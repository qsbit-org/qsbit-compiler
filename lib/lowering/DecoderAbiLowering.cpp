#include "lowering/DecoderAbiLowering.hpp"
#include "lowering/InstructionBuilder.hpp"
#include <llvm/IR/Module.h>
#include <qsbit/contracts/decoder.hpp>
#include <stdexcept>
namespace qsbit {
using lowering::assembly;
namespace registers = contract::decoder;
[[noreturn]] static void fail(const std::string &s) {
  throw std::runtime_error("adaptive QIR: " + s);
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
void lowerDecoderABI(llvm::Module &module, const Target &target) {
  for (const auto *name : {"reset_decoder_ui64", "enqueue_syndromes_ui64", "get_corrections_ui64",
                           "decoder_ready_ui64"}) {
    auto *fn = module.getFunction(name);
    if (!fn || fn->use_empty())
      continue;
    if (!target.hasDecoder)
      fail("decoder call requires target.decoding");
    if (!fn->isDeclaration())
      fail("decoder interface must be an external declaration");
    const bool readyQuery = llvm::StringRef(name) == "decoder_ready_ui64";
    const bool enqueue = llvm::StringRef(name).starts_with("enqueue");
    const bool reset = llvm::StringRef(name).starts_with("reset");
    const unsigned arity = enqueue ? 4 : (reset || readyQuery) ? 1 : 3;
    if (fn->arg_size() != arity || fn->isVarArg() ||
        (readyQuery ? !fn->getReturnType()->isIntegerTy(1)
                    : (enqueue || reset ? !fn->getReturnType()->isVoidTy()
                                        : !fn->getReturnType()->isIntegerTy(64))))
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
    if (!reset && !readyQuery) {
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
    store(b, target, contract::decoder::Select, fn->getArg(0));
    if (!enqueue && !reset && !readyQuery) {
      auto *widthChecked = llvm::BasicBlock::Create(c, "width_checked", fn);
      b.CreateCondBr(
          b.CreateICmpEQ(b.CreateZExt(load(b, target, contract::decoder::Count), b.getInt64Ty()),
                         fn->getArg(1)),
          widthChecked, bad);
      b.SetInsertPoint(widthChecked);
    }
    if (readyQuery) {
      // Match get_corrections: an unread result exists and no work remains.
      // This read never consumes the result or waits for decoder progress.
      auto *status = load(b, target, contract::decoder::Command);
      b.CreateRet(
          b.CreateICmpEQ(b.CreateAnd(status, b.getInt32(registers::Ready | registers::Busy)),
                         b.getInt32(registers::Ready)));
    } else if (enqueue) {
      store(b, target, contract::decoder::Count, fn->getArg(1));
      store(b, target, contract::decoder::DataLow, fn->getArg(2));
      store(b, target, contract::decoder::DataHigh, b.CreateLShr(fn->getArg(2), 32));
      store(b, target, contract::decoder::Tag, fn->getArg(3));
      store(b, target, contract::decoder::Command, b.getInt32(registers::Submit));
      b.CreateRetVoid();
    } else {
      if (reset)
        store(b, target, contract::decoder::Command, b.getInt32(registers::Reset));
      auto *poll = llvm::BasicBlock::Create(c, "poll", fn);
      auto *done = llvm::BasicBlock::Create(c, "done", fn);
      b.CreateBr(poll);
      b.SetInsertPoint(poll);
      auto *status = load(b, target, contract::decoder::Command);
      auto *ready = b.CreateICmpEQ(
          b.CreateAnd(status,
                      b.getInt32(reset ? registers::Busy : registers::Ready | registers::Busy)),
          b.getInt32(reset ? 0 : registers::Ready));
      b.CreateCondBr(ready, done, poll);
      b.SetInsertPoint(done);
      if (reset)
        b.CreateRetVoid();
      else {
        auto *lo = b.CreateZExt(load(b, target, contract::decoder::ResultLow), b.getInt64Ty());
        auto *hi = b.CreateShl(
            b.CreateZExt(load(b, target, contract::decoder::ResultHigh), b.getInt64Ty()), 32);
        auto *value = b.CreateOr(lo, hi);
        auto *clear = llvm::BasicBlock::Create(c, "clear", fn);
        auto *ret = llvm::BasicBlock::Create(c, "return", fn);
        b.CreateCondBr(b.CreateICmpNE(fn->getArg(2), b.getInt64(0)), clear, ret);
        b.SetInsertPoint(clear);
        store(b, target, contract::decoder::Command, b.getInt32(registers::Consume));
        b.CreateBr(ret);
        b.SetInsertPoint(ret);
        b.CreateRet(value);
      }
    }
  }
}

} // namespace qsbit
