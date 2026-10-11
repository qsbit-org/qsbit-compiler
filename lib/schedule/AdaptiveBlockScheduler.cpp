#include "model/Program.hpp"
#include "schedule/BlockScheduler.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
namespace qsbit {
OperationTiming BlockSchedule::reserve(std::span<const Mapping> mappings, bool measurement) {
  OperationTiming timing;
  if (mappings.size() > target_.stagingCapacity)
    throw std::runtime_error("schedule: group exceeds staging capacity");
  const bool conflict = std::any_of(mappings.begin(), mappings.end(), [&](const auto &mapping) {
    return ports_.contains(mapping.port) ||
           std::any_of(mapping.qubits.begin(), mapping.qubits.end(),
                       [&](auto q) { return qubits_.contains(q); });
  });
  if (mappings.size() > target_.stagingCapacity - commands_ || conflict) {
    timing.before = used_;
    used_ = commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  for (const auto &mapping : mappings) {
    ports_.insert(mapping.port);
    ++commands_;
    qubits_.insert(mapping.qubits.begin(), mapping.qubits.end());
    used_ = std::max(used_, durationCycles(mapping.duration, target_.tcuPeriod));
  }
  if (measurement) {
    timing.after = std::max(used_, target_.blockCycles);
    used_ = commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  return timing;
}
std::uint32_t BlockSchedule::finish() const { return std::max(used_, target_.blockCycles); }
AdaptiveSchedule scheduleAdaptive(const std::vector<BlockOperations> &blocks, const Target &target,
                                  bool batchOperations) {
  AdaptiveSchedule result;
  std::set<OperationId> operationIds;
  std::set<std::pair<std::uint32_t, std::uint32_t>> codes;
  for (const auto &mapping : target.mappings)
    codes.emplace(mapping.port, mapping.codeword);
  std::map<std::vector<std::pair<std::uint32_t, std::uint32_t>>, Mapping> groups;
  for (const auto &operations : blocks) {
    BlockSchedule scheduler(target);
    ScheduledBlock block;
    for (std::size_t i = 0; i < operations.operations.size();) {
      const auto &first = operations.operations[i];
      const auto name = first.operation.name;
      const bool measurement = name == QuantumOp::MeasureZ;
      const bool batch = batchOperations && (measurement || name == QuantumOp::Reset);
      const auto limit = measurement ? std::min(target.stagingCapacity, target.resultCapacity)
                                     : target.stagingCapacity;
      if (limit == 0)
        throw std::runtime_error("schedule: no capacity for operation");
      std::vector<BlockOperation> members;
      std::vector<Mapping> actions;
      std::set<std::uint32_t> qubits, ports, outputs;
      for (std::size_t j = i; j < operations.operations.size() && members.size() < limit; ++j) {
        const auto &request = operations.operations[j];
        const auto &op = request.operation;
        if (j != i && (!batch || op.name != name || request.batchRegion != first.batchRegion))
          break;
        const auto &m = mapping(target, op.name, op.qubits);
        if (ports.contains(m.port) ||
            std::any_of(m.qubits.begin(), m.qubits.end(),
                        [&](auto q) { return qubits.contains(q); }) ||
            (op.result && outputs.contains(*op.result)))
          break;
        if (!operationIds.insert(request.id).second)
          throw std::logic_error("schedule: duplicate operation ID");
        members.push_back(request);
        actions.push_back(m);
        ports.insert(m.port);
        qubits.insert(m.qubits.begin(), m.qubits.end());
        if (op.result)
          outputs.insert(*op.result);
      }
      Mapping command = actions.front();
      if (members.size() > 1) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> key;
        for (const auto &action : actions)
          key.emplace_back(action.port, action.codeword);
        if (const auto existing = groups.find(key); existing != groups.end()) {
          command = existing->second;
        } else {
          bool found = false;
          for (std::uint32_t port = 0; port < target.ports && !found; ++port)
            for (std::uint32_t code = 0; code <= 65535; ++code)
              if (codes.emplace(port, code).second) {
                command.port = port;
                command.codeword = code;
                found = true;
                break;
              }
          if (!found)
            throw std::runtime_error("schedule: grouped codeword space exhausted");
          command.qubits.assign(qubits.begin(), qubits.end());
          for (const auto &action : actions)
            command.duration = std::max(command.duration, action.duration);
          groups.emplace(std::move(key), command);
          result.mappings.push_back({command, actions});
        }
      }
      const auto timing = scheduler.reserve(actions, measurement);
      for (std::size_t member = 0; member < members.size(); ++member)
        block.calls.emplace(members[member].id,
                            ScheduledCall{command, timing,
                                          member == 0 ? members : std::vector<BlockOperation>{},
                                          member == 0});
      i += members.size();
    }
    block.finish = scheduler.finish();
    if (!result.blocks.emplace(operations.id, std::move(block)).second)
      throw std::logic_error("schedule: duplicate block ID");
  }
  return result;
}
} // namespace qsbit
