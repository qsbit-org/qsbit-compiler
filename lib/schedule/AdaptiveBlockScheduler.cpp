#include "schedule/BlockScheduler.hpp"
#include <algorithm>
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
  used_ = std::max(
      used_, static_cast<std::uint32_t>((std::uint64_t(mapping.duration) + target_.tcuPeriod - 1) /
                                        target_.tcuPeriod));
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
  for (const auto &operations : blocks) {
    BlockSchedule scheduler(target);
    ScheduledBlock block;
    for (const auto &request : operations) {
      const auto &op = request.operation;
      const auto &m =
          mapping(target, op.name == QuantumOp::Reset ? QuantumOp::MeasureZ : op.name, op.qubits);
      ScheduledCall call{
          m, scheduler.reserve(m, op.name == QuantumOp::MeasureZ || op.name == QuantumOp::Reset),
          std::nullopt, 0};
      if (op.name == QuantumOp::Reset) {
        call.reset = mapping(target, QuantumOp::X, op.qubits);
        call.resetWait = std::max(target.blockCycles,
                                  (call.reset->duration + target.tcuPeriod - 1) / target.tcuPeriod);
      }
      block.calls.emplace(request.instruction, std::move(call));
    }
    block.finish = scheduler.finish();
    result.blocks.push_back(std::move(block));
  }
  return result;
}
} // namespace qsbit
