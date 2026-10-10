#include "model/Program.hpp"
#include "schedule/BlockScheduler.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
namespace qsbit {
OperationTiming BlockSchedule::reserve(const Mapping &mapping, bool measurement) {
  OperationTiming timing;
  if (commands_ == target_.stagingCapacity || ports_.contains(mapping.port) ||
      std::any_of(mapping.qubits.begin(), mapping.qubits.end(),
                  [&](auto q) { return qubits_.contains(q); })) {
    timing.before = used_;
    used_ = 0;
    commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  ports_.insert(mapping.port);
  ++commands_;
  qubits_.insert(mapping.qubits.begin(), mapping.qubits.end());
  used_ = std::max(used_, durationCycles(mapping.duration, target_.tcuPeriod));
  if (measurement) {
    timing.after = std::max(used_, target_.blockCycles);
    used_ = 0;
    commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  return timing;
}
std::uint32_t BlockSchedule::finish() const { return std::max(used_, target_.blockCycles); }
AdaptiveSchedule scheduleAdaptive(const std::vector<BlockOperations> &blocks,
                                  const Target &target) {
  AdaptiveSchedule result;
  std::set<OperationId> operationIds;
  for (const auto &operations : blocks) {
    BlockSchedule scheduler(target);
    ScheduledBlock block;
    for (const auto &request : operations.operations) {
      if (!operationIds.insert(request.id).second)
        throw std::logic_error("schedule: duplicate operation ID");
      const auto &op = request.operation;
      const auto &m =
          mapping(target, op.name == QuantumOp::Reset ? QuantumOp::MeasureZ : op.name, op.qubits);
      ScheduledCall call{
          m, scheduler.reserve(m, op.name == QuantumOp::MeasureZ || op.name == QuantumOp::Reset),
          std::nullopt, 0};
      if (op.name == QuantumOp::Reset) {
        call.reset = mapping(target, QuantumOp::X, op.qubits);
        call.resetWait =
            std::max(target.blockCycles, durationCycles(call.reset->duration, target.tcuPeriod));
      }
      block.calls.emplace(request.id, std::move(call));
    }
    block.finish = scheduler.finish();
    if (!result.blocks.emplace(operations.id, std::move(block)).second)
      throw std::logic_error("schedule: duplicate block ID");
  }
  return result;
}
} // namespace qsbit
