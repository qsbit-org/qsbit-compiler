#include "qsbit/Compiler.hpp"
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <set>
#include <stdexcept>

#include "qsbit/Adaptive.hpp"
namespace qsbit {
namespace {
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error("adaptive QIR: " + s); }
std::string gate(llvm::StringRef name) {
  if (name == "__quantum__qis__h__body")
    return "h";
  if (name == "__quantum__qis__x__body")
    return "x";
  if (name == "__quantum__qis__z__body")
    return "z";
  if (name == "__quantum__qis__cx__body" || name == "__quantum__qis__cnot__body")
    return "cx";
  if (name == "__quantum__qis__mz__body")
    return "measure";
  if (name == "__quantum__qis__reset__body")
    return "reset";
  return {};
}
bool readResult(llvm::StringRef name) {
  return name == "__quantum__rt__read_result" || name == "__quantum__qis__read_result__body";
}
llvm::Value *assembly(llvm::IRBuilder<> &b, llvm::StringRef text, llvm::StringRef constraints,
                      llvm::Type *result, llvm::ArrayRef<llvm::Value *> values) {
  std::vector<llvm::Type *> types;
  for (auto *v : values)
    types.push_back(v->getType());
  return b.CreateCall(
      llvm::InlineAsm::get(llvm::FunctionType::get(result, types, false), text, constraints, true),
      values);
}
void wait(llvm::IRBuilder<> &b, std::uint32_t cycles) {
  if (cycles)
    assembly(b, ".insn r 0x0b, 1, 0, x0, $0, x0", "r,~{memory}", b.getVoidTy(),
             {b.getInt32(cycles)});
}
void cw(llvm::IRBuilder<> &b, const Mapping &m) {
  assembly(b, ".insn r 0x0b, 0, 0, x0, $0, $1", "r,r,~{memory}", b.getVoidTy(),
           {b.getInt32(m.port), b.getInt32(m.codeword)});
}
llvm::Value *fmr(llvm::IRBuilder<> &b, std::uint32_t q) {
  return assembly(b, ".insn r 0x0b, 3, 0, $0, x" + std::to_string(q) + ", x0", "=r,~{memory}",
                  b.getInt32Ty(), {});
}
llvm::Value *reg(llvm::IRBuilder<> &b, const Target &target, unsigned offset) {
  return b.CreateIntToPtr(b.getInt32(target.decoderBase + offset), b.getPtrTy());
}
void store(llvm::IRBuilder<> &b, const Target &target, unsigned offset, llvm::Value *v) {
  b.CreateStore(b.CreateZExtOrTrunc(v, b.getInt32Ty()), reg(b, target, offset), true);
}
llvm::Value *load(llvm::IRBuilder<> &b, const Target &target, unsigned offset) {
  return b.CreateLoad(b.getInt32Ty(), reg(b, target, offset), true);
}
void decoderFunctions(llvm::Module &module, const Target &target) {
  for (const auto *name :
       {"reset_decoder_ui64", "enqueue_syndromes_ui64", "get_corrections_ui64"}) {
    auto *fn = module.getFunction(name);
    if (!fn || fn->use_empty())
      continue;
    if (target.decoding.empty())
      fail("decoder call requires target.decoding");
    if (!fn->isDeclaration())
      fail("decoder interface must be an external declaration");
    const bool enqueue = llvm::StringRef(name).starts_with("enqueue");
    const bool reset = llvm::StringRef(name).starts_with("reset");
    const unsigned arity = enqueue ? 4 : reset ? 1 : 3;
    if (fn->arg_size() != arity || fn->isVarArg() ||
        (enqueue || reset ? !fn->getReturnType()->isVoidTy()
                          : !fn->getReturnType()->isIntegerTy(64)))
      fail("invalid decoder function signature");
    for (auto &arg : fn->args())
      if (!arg.getType()->isIntegerTy(64))
        fail("decoder arguments must be i64");
    fn->setAttributes(llvm::AttributeList{});
    auto &c = module.getContext();
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(c, "entry", fn));
    // Reject values that cannot be represented by the device registers.
    auto *bad = llvm::BasicBlock::Create(c, "invalid", fn);
    auto *body = llvm::BasicBlock::Create(c, "body", fn);
    llvm::Value *valid = b.CreateICmpULE(fn->getArg(0), b.getInt64(UINT32_MAX));
    if (!reset) {
      valid = b.CreateAnd(valid, b.CreateICmpUGE(fn->getArg(1), b.getInt64(1)));
      valid = b.CreateAnd(valid, b.CreateICmpULE(fn->getArg(1), b.getInt64(64)));
      valid = b.CreateAnd(valid, b.CreateICmpULE(fn->getArg(enqueue ? 3 : 2),
                                                 b.getInt64(enqueue ? UINT32_MAX : 1)));
    }
    b.CreateCondBr(valid, body, bad);
    b.SetInsertPoint(bad);
    assembly(b, ".word 0", "~{memory}", b.getVoidTy(), {});
    b.CreateUnreachable();
    b.SetInsertPoint(body);
    store(b, target, 0, fn->getArg(0));
    if (!enqueue && !reset) {
      auto *widthChecked = llvm::BasicBlock::Create(c, "width_checked", fn);
      b.CreateCondBr(
          b.CreateICmpEQ(b.CreateZExt(load(b, target, 4), b.getInt64Ty()), fn->getArg(1)),
          widthChecked, bad);
      b.SetInsertPoint(widthChecked);
    }
    if (enqueue) {
      store(b, target, 4, fn->getArg(1));
      store(b, target, 8, fn->getArg(2));
      store(b, target, 12, b.CreateLShr(fn->getArg(2), 32));
      store(b, target, 16, fn->getArg(3));
      store(b, target, 20, b.getInt32(1));
      b.CreateRetVoid();
    } else {
      if (reset)
        store(b, target, 20, b.getInt32(2));
      auto *poll = llvm::BasicBlock::Create(c, "poll", fn);
      auto *done = llvm::BasicBlock::Create(c, "done", fn);
      b.CreateBr(poll);
      b.SetInsertPoint(poll);
      auto *status = load(b, target, 20);
      auto *ready =
          b.CreateICmpEQ(b.CreateAnd(status, b.getInt32(reset ? 2 : 3)), b.getInt32(reset ? 0 : 1));
      b.CreateCondBr(ready, done, poll);
      b.SetInsertPoint(done);
      if (reset)
        b.CreateRetVoid();
      else {
        auto *lo = b.CreateZExt(load(b, target, 24), b.getInt64Ty());
        auto *hi = b.CreateShl(b.CreateZExt(load(b, target, 28), b.getInt64Ty()), 32);
        auto *value = b.CreateOr(lo, hi);
        auto *clear = llvm::BasicBlock::Create(c, "clear", fn);
        auto *ret = llvm::BasicBlock::Create(c, "return", fn);
        b.CreateCondBr(b.CreateICmpNE(fn->getArg(2), b.getInt64(0)), clear, ret);
        b.SetInsertPoint(clear);
        store(b, target, 20, b.getInt32(3));
        b.CreateBr(ret);
        b.SetInsertPoint(ret);
        b.CreateRet(value);
      }
    }
  }
}
llvm::GlobalVariable *storage(llvm::Module &module, llvm::Type *type, llvm::StringRef name) {
  if (module.getNamedValue(name))
    fail("reserved global name: " + name.str());
  auto *value = llvm::cast<llvm::GlobalVariable>(module.getOrInsertGlobal(name, type));
  value->setInitializer(llvm::Constant::getNullValue(type));
  value->setLinkage(llvm::GlobalValue::InternalLinkage);
  return value;
}
} // namespace
std::unique_ptr<llvm::Module> lowerAdaptive(const Program &program, const Target &target,
                                            llvm::LLVMContext &context) {
  auto module = prepareAdaptive(program, context);
  auto *entry = module->getFunction(program.entry);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *resultsType = llvm::ArrayType::get(i32, std::max(1U, program.results));
  auto *results = storage(*module, resultsType, "__qsbit_measurement_results");
  auto *outputCount = storage(*module, i32, "__qsbit_output_count");
  std::vector<llvm::BasicBlock *> blocks;
  for (auto &block : *entry)
    blocks.push_back(&block);
  for (auto *block : blocks) {
    std::vector<llvm::Instruction *> original;
    for (auto &i : *block)
      original.push_back(&i);
    BlockSchedule schedule(target);
    for (auto *instruction : original) {
      llvm::IRBuilder<> b(instruction);
      auto *call = llvm::dyn_cast<llvm::CallInst>(instruction);
      if (!call) {
        auto *type = instruction->getType();
        if (!type->isVoidTy() && (!type->isIntegerTy() || type->getIntegerBitWidth() > 64))
          fail("unsupported classical value type");
        const auto opcode = instruction->getOpcode();
        if (opcode != llvm::Instruction::Br && opcode != llvm::Instruction::Ret &&
            opcode != llvm::Instruction::PHI && opcode != llvm::Instruction::ICmp &&
            opcode != llvm::Instruction::Select && opcode != llvm::Instruction::ZExt &&
            opcode != llvm::Instruction::SExt && opcode != llvm::Instruction::Trunc &&
            opcode != llvm::Instruction::Add && opcode != llvm::Instruction::Sub &&
            opcode != llvm::Instruction::And && opcode != llvm::Instruction::Or &&
            opcode != llvm::Instruction::Xor && opcode != llvm::Instruction::Shl &&
            opcode != llvm::Instruction::LShr && opcode != llvm::Instruction::AShr)
          fail("unsupported classical instruction: " + std::string(instruction->getOpcodeName()));
        continue;
      }
      auto name = call->getCalledFunction()->getName();
      const auto operation = gate(name);
      const auto resultPointer = [&](llvm::Value *v) {
        return b.CreateInBoundsGEP(resultsType, results,
                                   {b.getInt32(0), b.getInt32(resourceIndex(v, program.results))});
      };
      if (!operation.empty()) {
        const unsigned arity = operation == "cx" || operation == "measure" ? 2 : 1;
        if (call->arg_size() != arity || !call->getType()->isVoidTy())
          fail("invalid quantum call signature");
        std::vector<std::uint32_t> qubits{resourceIndex(call->getArgOperand(0), program.qubits)};
        if (operation == "cx")
          qubits.push_back(resourceIndex(call->getArgOperand(1), program.qubits));
        if (operation == "cx" && qubits[0] == qubits[1])
          fail("CX requires distinct qubits");
        const auto &m = mapping(target, operation == "reset" ? "measure" : operation, qubits);
        const auto timing = schedule.reserve(m, operation == "measure" || operation == "reset");
        wait(b, timing.before);
        cw(b, m);
        if (operation == "measure" || operation == "reset") {
          auto *value = fmr(b, qubits[0]);
          if (operation == "measure")
            b.CreateStore(value, resultPointer(call->getArgOperand(1)));
          wait(b, timing.after);
          if (operation == "reset") {
            const auto &x = mapping(target, "x", qubits);
            // Both branches reserve the same timing point.
            const auto text = "beqz $0, 1f\n.insn r 0x0b, 0, 0, x0, $1, $2\n1:";
            assembly(b, text, "r,r,r,~{memory}", b.getVoidTy(),
                     {value, b.getInt32(x.port), b.getInt32(x.codeword)});
            wait(b, std::max(target.blockCycles,
                             (x.duration + target.tcuPeriod - 1) / target.tcuPeriod));
          }
        }
        call->eraseFromParent();
      } else if (readResult(name)) {
        if (call->arg_size() != 1 || !call->getType()->isIntegerTy(1))
          fail("invalid result read signature");
        call->replaceAllUsesWith(b.CreateICmpNE(
            b.CreateLoad(i32, resultPointer(call->getArgOperand(0))), b.getInt32(0)));
        call->eraseFromParent();
      } else if (name == "__quantum__rt__result_record_output" ||
                 name == "__quantum__rt__bool_record_output") {
        if (call->arg_size() != 2 || !call->getType()->isVoidTy())
          fail("invalid output signature");
        if (!llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(1)))
          fail("adaptive output labels must be null");
        auto *index = b.CreateLoad(i32, outputCount);
        // The output buffer has 16383 words; wrap is forbidden.
        assembly(b, "li t6, 16383\nbltu $0, t6, 1f\n.word 0\n1:", "r,~{t6},~{memory}",
                 b.getVoidTy(), {index});
        auto *address = b.CreateAdd(b.getInt32(0x10004), b.CreateShl(index, 2));
        llvm::Value *value = nullptr;
        if (name == "__quantum__rt__result_record_output")
          value = b.CreateLoad(i32, resultPointer(call->getArgOperand(0)));
        else {
          if (!call->getArgOperand(0)->getType()->isIntegerTy(1))
            fail("boolean output requires i1");
          value = b.CreateZExtOrTrunc(call->getArgOperand(0), i32);
        }
        b.CreateStore(value, b.CreateIntToPtr(address, b.getPtrTy()), true);
        auto *next = b.CreateAdd(index, b.getInt32(1));
        b.CreateStore(next, outputCount);
        b.CreateStore(next, b.CreateIntToPtr(b.getInt32(0x10000), b.getPtrTy()), true);
        call->eraseFromParent();
      } else if (name == "__quantum__rt__initialize") {
        if (call->arg_size() != 1 || !call->getType()->isVoidTy() ||
            !llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(0)))
          fail("invalid initialize");
        call->eraseFromParent();
      } else if (name != "reset_decoder_ui64" && name != "enqueue_syndromes_ui64" &&
                 name != "get_corrections_ui64") {
        fail("unsupported call: " + name.str());
      } else {
        call->setAttributes(llvm::AttributeList{});
      }
    }
    llvm::IRBuilder<> b(block->getTerminator());
    wait(b, schedule.finish());
    if (llvm::isa<llvm::ReturnInst>(block->getTerminator())) {
      assembly(b, "li a0, 0\nli a7, 93\necall", "~{a0},~{a7},~{memory}", b.getVoidTy(), {});
      b.CreateUnreachable();
      block->getTerminator()->eraseFromParent();
    }
  }
  decoderFunctions(*module, target);
  llvm::IRBuilder<> initialize(&*entry->getEntryBlock().getFirstInsertionPt());
  initialize.CreateStore(initialize.getInt32(0), outputCount);
  initialize.CreateStore(
      initialize.getInt32(0),
      initialize.CreateIntToPtr(initialize.getInt32(0x10000), initialize.getPtrTy()), true);
  entry->setName("qsbit_entry");
  entry->setAttributes(llvm::AttributeList{});
  return module;
}
} // namespace qsbit
