#include "lowering/ControllerLowering.hpp"
#include "lowering/InstructionBuilder.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include <cstdint>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <map>
#include <qsbit/contracts/executable.hpp>
#include <qsbit/contracts/isa.hpp>
#include <string>
namespace qsbit {
void lowerStatic(llvm::Module &module, const StaticProgram &program, const StaticSchedule &plan) {
  auto &context = module.getContext();
  std::uint32_t index = 0;
  llvm::IRBuilder<> builder(module.getContext());
  auto *voidType = builder.getVoidTy();
  auto *i32 = builder.getInt32Ty();
  auto *function =
      llvm::Function::Create(llvm::FunctionType::get(voidType, false),
                             llvm::GlobalValue::ExternalLinkage, "qsbit_entry", module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  auto constant = [&](std::uint32_t value) { return builder.getInt32(value); };
  std::map<std::uint32_t, std::uint32_t> measurementTargets;
  std::map<std::uint32_t, llvm::Value *> bits;
  std::uint32_t cursor = 0;
  for (const auto &item : plan.operations) {
    lowering::assembly(builder,
                       contract::instruction(contract::ControlFunction::Wait, "x0, $0, x0"),
                       "r,~{memory}", voidType, {constant(item.cycle - cursor)});
    cursor = item.cycle;
    lowering::assembly(
        builder, contract::instruction(contract::ControlFunction::Codeword, "x0, $0, $1"),
        "r,r,~{memory}", voidType, {constant(item.mapping.port), constant(item.mapping.codeword)});
    if (item.operation.result)
      measurementTargets[*item.operation.result] = item.operation.qubits.front();
  }
  for (const auto &[id, targetQubit] : measurementTargets)
    bits[id] = lowering::assembly(
        builder,
        contract::instruction(contract::ControlFunction::FetchMeasurement, "$0, x") +
            std::to_string(targetQubit) + ", x0",
        "=r,~{memory}", i32, {});
  for (const auto &record : program.outputs) {
    if (record.kind == "result") {
      const auto address = contract::abi::Static.output_data + contract::abi::WordBytes * index++;
      auto *pointer = builder.CreateIntToPtr(constant(address), builder.getPtrTy());
      builder.CreateStore(bits.at(record.value), pointer, true);
    }
  }
  lowering::assembly(builder, "li a0, 0\nli a7, 93\necall", "~{a0},~{a7},~{memory}", voidType, {});
  builder.CreateUnreachable();
}
} // namespace qsbit
