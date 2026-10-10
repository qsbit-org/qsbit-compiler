#pragma once
#include "llvm_ir/OwnedModule.hpp"
#include "model/Program.hpp"
#include <llvm/IR/ValueHandle.h>
namespace qsbit {
struct AdaptiveIR {
  std::unique_ptr<OwnedModule> code = std::make_unique<OwnedModule>();
  std::map<BlockId, llvm::WeakVH> blocks;
  std::map<OperationId, llvm::WeakVH> operations;
  void validateBindings(const std::vector<BlockOperations> &, std::uint32_t qubits,
                        std::uint32_t results) const;
};
Program readAdaptive(std::unique_ptr<AdaptiveIR>);
void prepareAdaptive(Program &);
std::uint32_t resourceIndex(llvm::Value *, std::uint32_t);
std::optional<QuantumOp> quantumOperation(llvm::StringRef);
} // namespace qsbit
