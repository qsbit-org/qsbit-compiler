#include "schedule/ScheduledProgram.hpp"
#include <algorithm>
#include <stdexcept>

namespace qsbit {
Schedule schedule(const Program &program, const Target &target) {
  if (program.qubits > target.qubits)
    throw std::runtime_error("schedule: target has too few qubits");
  if (program.adaptive())
    return scheduleAdaptive(std::get<AdaptiveProgram>(program.body).blocks, target);
  const auto &body = std::get<StaticProgram>(program.body);
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
    auto mapping = std::find_if(target.mappings.begin(), target.mappings.end(), [&](const auto &m) {
      return m.operation == op.name && m.qubits == op.qubits;
    });
    if (mapping == target.mappings.end())
      throw std::runtime_error("schedule: no target mapping for " +
                               std::string(operation_name(op.name)));
    result.operations.push_back({op, *mapping, cycle});
    cycle += (mapping->duration + target.tcuPeriod - 1) / target.tcuPeriod;
  }
  return result;
}
} // namespace qsbit
