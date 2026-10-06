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
std::unique_ptr<llvm::Module> parse(llvm::StringRef input, llvm::LLVMContext &context) {
  llvm::SMDiagnostic error;
  auto module = llvm::parseIR(llvm::MemoryBufferRef(input, "adaptive"), error, context);
  if (!module)
    fail(error.getMessage().str());
  if (llvm::verifyModule(*module, &llvm::errs()))
    fail("invalid LLVM IR");
  return module;
}
} // namespace
std::uint32_t resourceIndex(llvm::Value *value, std::uint32_t limit) {
  std::uint64_t index = 0;
  if (!llvm::isa<llvm::ConstantPointerNull>(value)) {
    auto *expr = llvm::dyn_cast<llvm::ConstantExpr>(value);
    if (!expr || expr->getOpcode() != llvm::Instruction::IntToPtr)
      fail("qubit and result identifiers must be static");
    auto *number = llvm::dyn_cast<llvm::ConstantInt>(expr->getOperand(0));
    if (!number || number->getValue().getActiveBits() > 32)
      fail("invalid resource identifier");
    index = number->getZExtValue();
  }
  if (index >= limit)
    fail("resource identifier exceeds declared count");
  return static_cast<std::uint32_t>(index);
}
namespace {
bool readResult(llvm::StringRef name) {
  return name == "__quantum__rt__read_result" || name == "__quantum__qis__read_result__body";
}
} // namespace
Program readAdaptive(llvm::Module &input) {
  auto *module = &input;
  Program program;
  llvm::Function *entry = nullptr;
  for (auto &fn : *module)
    if (fn.hasFnAttribute("entry_point")) {
      if (entry)
        fail("multiple entry points");
      entry = &fn;
    }
  if (!entry || !entry->arg_empty() || entry->isVarArg() || entry->isDeclaration() ||
      !entry->getReturnType()->isVoidTy())
    fail("entry must be void with no arguments");
  program.entry = entry->getName().str();
  if (module->getModuleFlag("qir_major_version") || module->getModuleFlag("qir_minor_version")) {
    auto *major = module->getModuleFlag("qir_major_version");
    auto *v = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(major);
    auto *minor = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
        module->getModuleFlag("qir_minor_version"));
    if (!v || !minor || !v->equalsInt(1) || !minor->isZero())
      fail("only QIR 1.0 is supported");
  }
  auto count = [&](llvm::StringRef name) {
    std::uint32_t value = 0;
    auto attr = entry->getFnAttribute(name);
    if (!attr.isStringAttribute() || attr.getValueAsString().getAsInteger(10, value) ||
        value > 65536)
      fail("invalid " + name.str());
    return value;
  };
  program.qubits = count("required_num_qubits");
  program.results = count("required_num_results");
  if (program.qubits > 32)
    fail("target supports at most 32 qubits");
  if (!module->getModuleInlineAsm().empty())
    fail("module assembly is unsupported");
  for (const auto *flag : {"dynamic_qubit_management", "dynamic_result_management"}) {
    auto *md = module->getModuleFlag(flag);
    if (md) {
      auto *value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(md);
      if (!value || !value->isZero())
        fail("dynamic allocation is unsupported");
    }
  }
  program.dialect = "qir-adaptive-static-resources";
  for (auto a : entry->getAttributes().getFnAttrs())
    if (a.isStringAttribute())
      program.attributes[a.getKindAsString().str()] = a.getValueAsString().str();
  llvm::raw_string_ostream out(program.adaptiveIR);
  module->print(out, nullptr);
  return program;
}

std::unique_ptr<llvm::Module> prepareAdaptive(const Program &program, llvm::LLVMContext &context) {
  auto module = parse(program.adaptiveIR, context);
  auto *entry = module->getFunction(program.entry);
  for (auto &global : module->globals())
    if (!global.isConstant())
      fail("mutable input globals are unsupported");
  // Inline acyclic helper calls before scheduling the controller program.
  std::set<llvm::Function *> visiting, visited;
  std::function<void(llvm::Function *)> inlineCalls = [&](llvm::Function *fn) {
    if (visiting.contains(fn))
      fail("recursive functions are unsupported");
    if (visited.contains(fn))
      return;
    visiting.insert(fn);
    std::vector<llvm::CallInst *> calls;
    for (auto &block : *fn)
      for (auto &i : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&i)) {
          auto *callee = call->getCalledFunction();
          if (!callee || call->isInlineAsm() || call->hasOperandBundles() ||
              call->isMustTailCall() || call->getCallingConv() != llvm::CallingConv::C)
            fail("unsupported call semantics");
          if (!callee->isDeclaration()) {
            inlineCalls(callee);
            calls.push_back(call);
          }
        }
    for (auto *call : calls) {
      llvm::InlineFunctionInfo info;
      if (!llvm::InlineFunction(*call, info).isSuccess())
        fail("helper could not be inlined");
    }
    visiting.erase(fn);
    visited.insert(fn);
  };
  inlineCalls(entry);
  std::vector<llvm::Function *> unused;
  for (auto &fn : *module)
    if (&fn != entry && !fn.isDeclaration())
      unused.push_back(&fn);
  for (auto *fn : unused)
    fn->dropAllReferences();
  for (auto *fn : unused)
    fn->eraseFromParent();
  // Every result read must be preceded by a measurement on every incoming path.
  std::set<std::uint32_t> all;
  for (std::uint32_t r = 0; r < program.results; ++r)
    all.insert(r);
  std::map<llvm::BasicBlock *, std::set<std::uint32_t>> available;
  for (auto &block : *entry)
    available[&block] = &block == &entry->getEntryBlock() ? std::set<std::uint32_t>{} : all;
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto &block : *entry) {
      auto assigned = &block == &entry->getEntryBlock() || llvm::pred_empty(&block)
                          ? std::set<std::uint32_t>{}
                          : all;
      for (auto *pred : llvm::predecessors(&block)) {
        std::set<std::uint32_t> intersection;
        std::set_intersection(assigned.begin(), assigned.end(), available[pred].begin(),
                              available[pred].end(),
                              std::inserter(intersection, intersection.end()));
        assigned = std::move(intersection);
      }
      for (auto &i : block)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&i))
          if (call->getCalledFunction()->getName() == "__quantum__qis__mz__body" &&
              call->arg_size() == 2)
            assigned.insert(resourceIndex(call->getArgOperand(1), program.results));
      if (available[&block] != assigned) {
        available[&block] = std::move(assigned);
        changed = true;
      }
    }
  }
  for (auto &block : *entry) {
    auto assigned = &block == &entry->getEntryBlock() || llvm::pred_empty(&block)
                        ? std::set<std::uint32_t>{}
                        : all;
    for (auto *pred : llvm::predecessors(&block)) {
      std::set<std::uint32_t> intersection;
      std::set_intersection(assigned.begin(), assigned.end(), available[pred].begin(),
                            available[pred].end(), std::inserter(intersection, intersection.end()));
      assigned = std::move(intersection);
    }
    for (auto &i : block)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&i)) {
        auto name = call->getCalledFunction()->getName();
        if (name == "__quantum__qis__mz__body" && call->arg_size() == 2)
          assigned.insert(resourceIndex(call->getArgOperand(1), program.results));
        else if ((readResult(name) || name == "__quantum__rt__result_record_output") &&
                 call->arg_size())
          if (!assigned.contains(resourceIndex(call->getArgOperand(0), program.results)))
            fail("read of an unmeasured result");
      }
  }
  return module;
}
} // namespace qsbit
