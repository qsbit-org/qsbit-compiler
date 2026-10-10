#include "qir/AdaptiveIR.hpp"
namespace qsbit {
std::optional<QuantumOp> quantumOperation(llvm::StringRef name) {
  if (name == "__quantum__qis__h__body")
    return QuantumOp::H;
  if (name == "__quantum__qis__x__body")
    return QuantumOp::X;
  if (name == "__quantum__qis__z__body")
    return QuantumOp::Z;
  if (name == "__quantum__qis__s__body")
    return QuantumOp::S;
  if (name == "__quantum__qis__s__adj")
    return QuantumOp::Sdg;
  if (name == "__quantum__qis__t__body")
    return QuantumOp::T;
  if (name == "__quantum__qis__t__adj")
    return QuantumOp::Tdg;
  if (name == "__quantum__qis__cx__body" || name == "__quantum__qis__cnot__body")
    return QuantumOp::Cx;
  if (name == "__quantum__qis__mz__body")
    return QuantumOp::MeasureZ;
  if (name == "__quantum__qis__reset__body")
    return QuantumOp::Reset;
  return {};
}
} // namespace qsbit
