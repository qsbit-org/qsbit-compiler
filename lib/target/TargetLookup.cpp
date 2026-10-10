#include "target/TargetModel.hpp"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
namespace qsbit {
std::uint32_t durationCycles(std::uint32_t duration, std::uint32_t period) {
  if (period == 0)
    throw std::runtime_error("target: TCU period must be positive");
  return static_cast<std::uint32_t>((std::uint64_t(duration) + period - 1) / period);
}
const Mapping &mapping(const Target &target, QuantumOp name,
                       const std::vector<std::uint32_t> &qubits) {
  for (const auto &item : target.mappings)
    if (item.operation == name && item.qubits == qubits)
      return item;
  throw std::runtime_error("target: no target mapping for " + std::string(operation_name(name)));
}
} // namespace qsbit
