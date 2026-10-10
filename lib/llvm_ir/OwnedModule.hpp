#pragma once
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>

namespace qsbit {
struct OwnedModule {
  std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> module;
  OwnedModule() = default;
  OwnedModule(const OwnedModule &) = delete;
  OwnedModule &operator=(const OwnedModule &) = delete;
};
} // namespace qsbit
