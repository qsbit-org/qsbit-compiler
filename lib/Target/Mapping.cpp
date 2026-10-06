#include "qsbit/Adaptive.hpp"
#include <stdexcept>
namespace qsbit {
const Mapping &mapping(const Target &target, llvm::StringRef name,
                       const std::vector<std::uint32_t> &qubits) {
  for (const auto &item : target.mappings)
    if (llvm::StringRef(item.operation) == name && item.qubits == qubits)
      return item;
  throw std::runtime_error("target: no target mapping for " + name.str());
}
} // namespace qsbit
