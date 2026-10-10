#pragma once
#include "target/TargetModel.hpp"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <qsbit/contracts/isa.hpp>
namespace qsbit::lowering {
inline llvm::Value *assembly(llvm::IRBuilder<> &b, llvm::StringRef text,
                             llvm::StringRef constraints, llvm::Type *result,
                             llvm::ArrayRef<llvm::Value *> values) {
  std::vector<llvm::Type *> types;
  for (auto *v : values)
    types.push_back(v->getType());
  return b.CreateCall(
      llvm::InlineAsm::get(llvm::FunctionType::get(result, types, false), text, constraints, true),
      values);
}
inline void wait(llvm::IRBuilder<> &b, std::uint32_t cycles) {
  if (cycles)
    assembly(b, contract::instruction(contract::ControlFunction::Wait, "x0, $0, x0"), "r,~{memory}",
             b.getVoidTy(), {b.getInt32(cycles)});
}
inline void waitForFeedback(llvm::IRBuilder<> &b) {
  assembly(b, contract::instruction(contract::ControlFunction::Wait, "x0, x0, x0"), "~{memory}",
           b.getVoidTy(), {});
}
inline void cw(llvm::IRBuilder<> &b, const Mapping &m) {
  assembly(b, contract::instruction(contract::ControlFunction::Codeword, "x0, $0, $1"),
           "r,r,~{memory}", b.getVoidTy(), {b.getInt32(m.port), b.getInt32(m.codeword)});
}
inline llvm::Value *fmr(llvm::IRBuilder<> &b, std::uint32_t q) {
  return assembly(b,
                  contract::instruction(contract::ControlFunction::FetchMeasurement, "$0, x") +
                      std::to_string(q) + ", x0",
                  "=r,~{memory}", b.getInt32Ty(), {});
}

} // namespace qsbit::lowering
