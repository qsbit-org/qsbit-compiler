#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace qsbit {
StaticSchedule scheduleStatic(const StaticProgram &body, const Target &target) {
  if (body.operations.size() > MaxOperations || body.operations.size() > target.timingCapacity ||
      body.operations.size() > target.eventCapacity)
    throw std::runtime_error("schedule: program exceeds preload queue capacity");
  const auto measurements = std::count_if(body.operations.begin(), body.operations.end(),
                                          [](const auto &op) { return op.result.has_value(); });
  if (measurements > target.resultCapacity)
    throw std::runtime_error("schedule: terminal measurements exceed delivery capacity");
  StaticSchedule result;
  std::uint32_t cycle = 1;
  for (const auto &op : body.operations) {
    const auto &mapped = mapping(target, op.name, op.qubits);
    result.operations.push_back({op, mapped, cycle});
    cycle += durationCycles(mapped.duration, target.tcuPeriod);
  }
  return result;
}
} // namespace qsbit
