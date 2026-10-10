#include "target/TargetModel.hpp"
#include <stdexcept>
namespace qsbit {
const Mapping &mapping(const Target &target, QuantumOp name,
                       const std::vector<std::uint32_t> &qubits) {
  for (const auto &item : target.mappings)
    if (item.operation == name && item.qubits == qubits)
      return item;
  throw std::runtime_error("target: no target mapping for " + std::string(operation_name(name)));
}
} // namespace qsbit
