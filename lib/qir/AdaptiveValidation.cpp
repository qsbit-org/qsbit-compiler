#include "qir/AdaptiveValidation.hpp"
#include "model/Program.hpp"
#include "qir/AdaptiveIR.hpp"
#include <cstdint>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <stdexcept>
#include <string>
namespace qsbit {
bool isDecoderCall(llvm::StringRef name) {
  return name == "reset_decoder_ui64" || name == "enqueue_syndromes_ui64" ||
         name == "get_corrections_ui64" || name == "decoder_ready_ui64";
}
void validateAdaptiveCall(const llvm::CallInst &call, std::uint32_t qubits, std::uint32_t results) {
  const auto fail = [](const std::string &message) {
    throw std::runtime_error("adaptive QIR: " + message);
  };
  const auto *fn = call.getCalledFunction();
  const auto name = fn->getName();
  const auto signature = [&](unsigned arity, bool type) {
    if (call.arg_size() != arity || fn->isVarArg() || !type)
      fail(isDecoderCall(name) ? "invalid decoder function signature"
                               : "invalid call signature: " + name.str());
  };
  if (auto operation = quantumOperation(name)) {
    signature(*operation == QuantumOp::Cx || *operation == QuantumOp::MeasureZ ? 2 : 1,
              call.getType()->isVoidTy());
    auto first = resourceIndex(call.getArgOperand(0), qubits);
    if (*operation == QuantumOp::Cx && first == resourceIndex(call.getArgOperand(1), qubits))
      fail("CX requires distinct qubits");
    if (*operation == QuantumOp::MeasureZ)
      (void)resourceIndex(call.getArgOperand(1), results);
  } else if (name == "__quantum__rt__read_result" || name == "__quantum__qis__read_result__body") {
    signature(1, call.getType()->isIntegerTy(1));
    (void)resourceIndex(call.getArgOperand(0), results);
  } else if (name == "__quantum__rt__result_record_output" ||
             name == "__quantum__rt__bool_record_output") {
    signature(2, call.getType()->isVoidTy());
    if (name == "__quantum__rt__result_record_output")
      (void)resourceIndex(call.getArgOperand(0), results);
    else if (!call.getArgOperand(0)->getType()->isIntegerTy(1))
      fail("boolean output requires i1");
    llvm::StringRef label;
    if (!llvm::isa<llvm::ConstantPointerNull>(call.getArgOperand(1)) &&
        !llvm::getConstantStringInfo(call.getArgOperand(1), label))
      fail("adaptive output label must be a constant string or null");
  } else if (name == "__quantum__rt__initialize") {
    signature(1, call.getType()->isVoidTy());
    if (!llvm::isa<llvm::ConstantPointerNull>(call.getArgOperand(0)))
      fail("invalid initialize");
  } else if (isDecoderCall(name)) {
    const bool enqueue = name == "enqueue_syndromes_ui64";
    const bool reset = name == "reset_decoder_ui64";
    const bool ready = name == "decoder_ready_ui64";
    signature(enqueue            ? 4
              : (reset || ready) ? 1
                                 : 3,
              ready              ? call.getType()->isIntegerTy(1)
              : enqueue || reset ? call.getType()->isVoidTy()
                                 : call.getType()->isIntegerTy(64));
    for (const auto &argument : call.args())
      if (!argument->getType()->isIntegerTy(64))
        fail("decoder arguments must be i64");
  } else {
    fail("unsupported call: " + name.str());
  }
}
} // namespace qsbit
