#pragma once
#include "model/Program.hpp"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
namespace qsbit {
struct AdaptiveIR {
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
};
Program readAdaptive(std::unique_ptr<AdaptiveIR>);
void prepareAdaptive(Program &);
std::uint32_t resourceIndex(llvm::Value *, std::uint32_t);
std::optional<QuantumOp> quantumOperation(llvm::StringRef);
} // namespace qsbit
