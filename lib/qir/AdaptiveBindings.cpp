#include "model/Program.hpp"
#include "qir/AdaptiveIR.hpp"
#include <cstddef>
#include <cstdint>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <set>
#include <stdexcept>
#include <vector>
namespace qsbit {
void AdaptiveIR::validateBindings(const std::vector<BlockOperations> &view, std::uint32_t qubits,
                                  std::uint32_t results) const {
  const auto invalid = [] {
    throw std::logic_error("adaptive IR changed; rebuild operation bindings");
  };
  if (!code || !code->module || view.size() != blocks.size())
    invalid();
  std::set<BlockId> seenBlocks;
  std::set<OperationId> seenOperations;
  for (const auto &record : view) {
    auto blockEntry = blocks.find(record.id);
    auto *block = blockEntry == blocks.end() ? nullptr
                                             : llvm::dyn_cast_or_null<llvm::BasicBlock>(
                                                   static_cast<llvm::Value *>(blockEntry->second));
    if (!seenBlocks.insert(record.id).second || !block ||
        block->getModule() != code->module.get() || block->getParent()->size() != view.size())
      invalid();
    const llvm::Instruction *previous = nullptr;
    for (const auto &operation : record.operations) {
      if (!seenOperations.insert(operation.id).second)
        invalid();
      auto entry = operations.find(operation.id);
      auto *call =
          entry == operations.end()
              ? nullptr
              : llvm::dyn_cast_or_null<llvm::CallInst>(static_cast<llvm::Value *>(entry->second));
      if (!call || call->getParent() != block || !call->getCalledFunction() ||
          call->arg_size() !=
              operation.operation.qubits.size() + (operation.operation.result ? 1U : 0U) ||
          quantumOperation(call->getCalledFunction()->getName()) != operation.operation.name ||
          (previous && !previous->comesBefore(call)))
        invalid();
      previous = call;
      if (operation.operation.result &&
          resourceIndex(call->getArgOperand(1), results) != *operation.operation.result)
        invalid();
      for (std::size_t i = 0; i < operation.operation.qubits.size(); ++i)
        if (i >= call->arg_size() ||
            resourceIndex(call->getArgOperand(i), qubits) != operation.operation.qubits[i])
          invalid();
    }
    std::size_t actual = 0;
    for (const auto &instruction : *block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        if (call->getCalledFunction() && quantumOperation(call->getCalledFunction()->getName()))
          ++actual;
    if (actual != record.operations.size())
      invalid();
  }
  if (seenOperations.size() != operations.size())
    invalid();
}
} // namespace qsbit
