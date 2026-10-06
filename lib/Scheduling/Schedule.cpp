#include "qsbit/Compiler.hpp"
#include <algorithm>
#include <stdexcept>

namespace qsbit {
Schedule schedule(const Program &program, const Target &target) {
  if (program.qubits > target.qubits)
    throw std::runtime_error("schedule: target has too few qubits");
  if (!program.adaptiveIR.empty())
    return {};
  if (program.operations.size() > MaxOperations ||
      program.operations.size() > target.timingCapacity ||
      program.operations.size() > target.eventCapacity)
    throw std::runtime_error("schedule: program exceeds preload queue capacity");
  const auto measurements = std::count_if(program.operations.begin(), program.operations.end(),
                                          [](const auto &op) { return op.result.has_value(); });
  if (measurements > target.resultCapacity)
    throw std::runtime_error("schedule: terminal measurements exceed delivery capacity");
  Schedule result;
  std::uint32_t cycle = 1;
  for (const auto &op : program.operations) {
    auto mapping = std::find_if(target.mappings.begin(), target.mappings.end(), [&](const auto &m) {
      return m.operation == op.name && m.qubits == op.qubits;
    });
    if (mapping == target.mappings.end())
      throw std::runtime_error("schedule: no target mapping for " + op.name);
    result.operations.push_back({op, *mapping, cycle});
    cycle += (mapping->duration + target.tcuPeriod - 1) / target.tcuPeriod;
  }
  return result;
}
} // namespace qsbit
