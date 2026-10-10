#include "llvm_ir/OwnedModule.hpp"
#include "lowering/ControllerLowering.hpp"
#include "lowering/DecoderAbiLowering.hpp"
#include "lowering/InstructionBuilder.hpp"
#include "model/Program.hpp"
#include "qir/AdaptiveIR.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <algorithm>
#include <cstdint>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <map>
#include <memory>
#include <qsbit/contracts/executable.hpp>
#include <qsbit/contracts/isa.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qsbit {
using namespace lowering;
namespace {
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error("adaptive QIR: " + s); }
bool readResult(llvm::StringRef name) {
  return name == "__quantum__rt__read_result" || name == "__quantum__qis__read_result__body";
}
llvm::GlobalVariable *storage(llvm::Module &module, llvm::Type *type, llvm::StringRef name) {
  auto *value = llvm::cast<llvm::GlobalVariable>(module.getOrInsertGlobal(name, type));
  value->setInitializer(llvm::Constant::getNullValue(type));
  value->setLinkage(llvm::GlobalValue::InternalLinkage);
  return value;
}
} // namespace
std::unique_ptr<OwnedModule> lowerAdaptive(Program &program, const Target &target,
                                           const AdaptiveSchedule &plan) {
  auto &body = std::get<AdaptiveProgram>(program.body);
  body.ir->validateBindings(body.blocks, program.qubits, program.results);
  auto owner = std::move(body.ir->code);
  auto &context = *owner->context;
  auto &module = owner->module;
  auto *entry = module->getFunction(program.entry);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *resultsType = llvm::ArrayType::get(i32, std::max(1U, program.results));
  auto *results = storage(*module, resultsType, "__qsbit_measurement_results");
  auto *outputCount = storage(*module, i32, "__qsbit_output_count");
  std::map<const llvm::Value *, OperationId> operationIds;
  for (const auto &[id, value] : body.ir->operations)
    operationIds.emplace(value, id);
  for (const auto &[id, value] : body.ir->blocks) {
    auto *block = llvm::cast<llvm::BasicBlock>(static_cast<llvm::Value *>(value));
    const auto &scheduled = plan.blocks.at(id);
    std::vector<llvm::Instruction *> original;
    for (auto &i : *block)
      original.push_back(&i);
    for (auto *instruction : original) {
      llvm::IRBuilder<> b(instruction);
      auto *call = llvm::dyn_cast<llvm::CallInst>(instruction);
      if (!call)
        continue;
      auto name = call->getCalledFunction()->getName();
      const auto operation = quantumOperation(name);
      const auto resultPointer = [&](llvm::Value *v) {
        return b.CreateInBoundsGEP(resultsType, results,
                                   {b.getInt32(0), b.getInt32(resourceIndex(v, program.results))});
      };
      if (operation.has_value()) {
        std::vector<std::uint32_t> qubits{resourceIndex(call->getArgOperand(0), program.qubits)};
        if (operation == QuantumOp::Cx)
          qubits.push_back(resourceIndex(call->getArgOperand(1), program.qubits));
        const auto &callPlan = scheduled.calls.at(operationIds.at(call));
        const auto &m = callPlan.mapping;
        const auto timing = callPlan.timing;
        wait(b, timing.before);
        cw(b, m);
        if (operation == QuantumOp::MeasureZ || operation == QuantumOp::Reset) {
          waitForFeedback(b);
          auto *value = fmr(b, qubits[0]);
          if (operation == QuantumOp::MeasureZ)
            b.CreateStore(value, resultPointer(call->getArgOperand(1)));
          wait(b, timing.after);
          if (operation == QuantumOp::Reset) {
            const auto &x = *callPlan.reset;
            // Both branches reserve the same timing point.
            const auto text =
                "beqz $0, 1f\n" +
                contract::instruction(contract::ControlFunction::Codeword, "x0, $1, $2") + "\n1:";
            assembly(b, text, "r,r,r,~{memory}", b.getVoidTy(),
                     {value, b.getInt32(x.port), b.getInt32(x.codeword)});
            wait(b, callPlan.resetWait);
          }
        }
        call->eraseFromParent();
      } else if (readResult(name)) {
        call->replaceAllUsesWith(b.CreateICmpNE(
            b.CreateLoad(i32, resultPointer(call->getArgOperand(0))), b.getInt32(0)));
        call->eraseFromParent();
      } else if (name == "__quantum__rt__result_record_output" ||
                 name == "__quantum__rt__bool_record_output") {
        auto *index = b.CreateLoad(i32, outputCount);
        // Reject output beyond the ABI buffer.
        assembly(b,
                 "li t6, " + std::to_string(contract::abi::Adaptive.output_capacity) +
                     "\nbltu $0, t6, 1f\n.word 0\n1:",
                 "r,~{t6},~{memory}", b.getVoidTy(), {index});
        auto *address = b.CreateAdd(b.getInt32(contract::abi::Adaptive.output_data),
                                    b.CreateMul(index, b.getInt32(contract::abi::WordBytes)));
        llvm::Value *value = nullptr;
        if (name == "__quantum__rt__result_record_output")
          value = b.CreateLoad(i32, resultPointer(call->getArgOperand(0)));
        else {
          value = b.CreateZExtOrTrunc(call->getArgOperand(0), i32);
        }
        b.CreateStore(value, b.CreateIntToPtr(address, b.getPtrTy()), true);
        auto *next = b.CreateAdd(index, b.getInt32(1));
        b.CreateStore(next, outputCount);
        b.CreateStore(
            next, b.CreateIntToPtr(b.getInt32(contract::abi::Adaptive.output_count), b.getPtrTy()),
            true);
        call->eraseFromParent();
      } else if (name == "__quantum__rt__initialize") {
        call->eraseFromParent();
      } else if (name != "reset_decoder_ui64" && name != "enqueue_syndromes_ui64" &&
                 name != "get_corrections_ui64" && name != "decoder_ready_ui64") {
        fail("unsupported call: " + name.str());
      } else {
        call->setAttributes(llvm::AttributeList{});
        waitForFeedback(b);
      }
    }
    llvm::IRBuilder<> b(block->getTerminator());
    wait(b, scheduled.finish);
    if (llvm::isa<llvm::ReturnInst>(block->getTerminator())) {
      auto *returned = llvm::cast<llvm::ReturnInst>(block->getTerminator())->getReturnValue();
      if (returned) {
        if (!llvm::isa<llvm::ConstantInt>(returned)) {
          auto *valid = b.CreateICmpULE(returned, b.getInt64(63));
          assembly(b, "bnez $0, 1f\n.word 0\n1:", "r,~{memory}", b.getVoidTy(),
                   {b.CreateZExt(valid, i32)});
        }
      }
      auto *status = returned ? b.CreateTruncOrBitCast(returned, i32) : b.getInt32(0);
      assembly(b, "mv a0, $0\nli a7, 93\necall", "r,~{a0},~{a7},~{memory}", b.getVoidTy(),
               {status});
      b.CreateUnreachable();
      block->getTerminator()->eraseFromParent();
    }
  }
  lowerDecoderABI(*module, target);
  llvm::IRBuilder<> initialize(&*entry->getEntryBlock().getFirstInsertionPt());
  initialize.CreateStore(initialize.getInt32(0), outputCount);
  initialize.CreateStore(
      initialize.getInt32(0),
      initialize.CreateIntToPtr(initialize.getInt32(contract::abi::Adaptive.output_count),
                                initialize.getPtrTy()),
      true);
  entry->setName("qsbit_entry");
  entry->setAttributes(llvm::AttributeList{});
  return owner;
}
} // namespace qsbit
