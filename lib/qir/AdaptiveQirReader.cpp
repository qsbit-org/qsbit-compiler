#include "model/Program.hpp"
#include "qir/AdaptiveIR.hpp"
#include "qir/AdaptiveValidation.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <llvm/IR/CFG.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Support/Casting.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qsbit {
namespace {
[[noreturn]] void fail(const std::string &s) { throw std::runtime_error("adaptive QIR: " + s); }
} // namespace
std::uint32_t resourceIndex(llvm::Value *value, std::uint32_t limit) {
  if (!value->getType()->isPointerTy())
    fail("qubit and result operands must be pointers");
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
Program readAdaptive(std::unique_ptr<AdaptiveIR> owned) {
  auto *module = owned->code->module.get();
  Program program;
  program.body.emplace<AdaptiveProgram>().ir = {owned.release(), [](AdaptiveIR *ir) { delete ir; }};
  llvm::Function *entry = nullptr;
  for (auto &fn : *module)
    if (fn.hasFnAttribute("entry_point")) {
      if (entry)
        fail("multiple entry points");
      entry = &fn;
    }
  if (!entry || !entry->arg_empty() || entry->isVarArg() || entry->isDeclaration() ||
      (!entry->getReturnType()->isVoidTy() && !entry->getReturnType()->isIntegerTy(64)))
    fail("entry must return void or i64 with no arguments");
  program.entry = entry->getName().str();
  if (module->getModuleFlag("qir_major_version") || module->getModuleFlag("qir_minor_version")) {
    auto *major = module->getModuleFlag("qir_major_version");
    auto *v = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(major);
    auto *minor = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
        module->getModuleFlag("qir_minor_version"));
    if (!v || !minor ||
        !((v->equalsInt(1) && minor->isZero()) || (v->equalsInt(2) && minor->equalsInt(1))))
      fail("supported Adaptive versions are QIR 1.0 and QIR 2.1");
    if (v->equalsInt(2) && !entry->getReturnType()->isIntegerTy(64))
      fail("QIR 2.1 entry must return i64");
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
  prepareAdaptive(program);
  return program;
}

void prepareAdaptive(Program &program) {
  auto &body = std::get<AdaptiveProgram>(program.body);
  auto &module = body.ir->code->module;
  auto *entry = module->getFunction(program.entry);
  for (const auto *name : {"__qsbit_measurement_results", "__qsbit_output_count"})
    if (module->getNamedValue(name))
      fail("reserved global name: " + std::string(name));
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
  for (const auto &block : *entry)
    for (const auto &instruction : block)
      if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction))
        validateAdaptiveCall(*call, program.qubits, program.results);
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
  body.blocks.clear();
  body.ir->blocks.clear();
  body.ir->operations.clear();
  std::uint32_t nextBlock = 0, nextOperation = 0;
  for (auto &block : *entry) {
    const BlockId blockId{nextBlock++};
    BlockOperations operations{blockId, {}};
    body.ir->blocks.emplace(blockId, &block);
    for (auto &instruction : block) {
      auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
      if (!call) {
        if (const auto *returned = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
          if (const auto *constant =
                  llvm::dyn_cast_or_null<llvm::ConstantInt>(returned->getReturnValue()))
            if (constant->getValue().ugt(63))
              fail("entry exit status must be in the range 0-63");
        auto *type = instruction.getType();
        if (!type->isVoidTy() && (!type->isIntegerTy() || type->getIntegerBitWidth() > 64))
          fail("unsupported classical value type");
        const auto opcode = instruction.getOpcode();
        if (opcode != llvm::Instruction::Br && opcode != llvm::Instruction::Ret &&
            opcode != llvm::Instruction::PHI && opcode != llvm::Instruction::ICmp &&
            opcode != llvm::Instruction::Select && opcode != llvm::Instruction::ZExt &&
            opcode != llvm::Instruction::SExt && opcode != llvm::Instruction::Trunc &&
            opcode != llvm::Instruction::Add && opcode != llvm::Instruction::Sub &&
            opcode != llvm::Instruction::And && opcode != llvm::Instruction::Or &&
            opcode != llvm::Instruction::Xor && opcode != llvm::Instruction::Shl &&
            opcode != llvm::Instruction::LShr && opcode != llvm::Instruction::AShr)
          fail("unsupported classical instruction: " + std::string(instruction.getOpcodeName()));
        continue;
      }

      if (call) {
        auto operation = quantumOperation(call->getCalledFunction()->getName());
        if (operation.has_value()) {
          std::vector<std::uint32_t> qubits{resourceIndex(call->getArgOperand(0), program.qubits)};
          if (operation == QuantumOp::Cx) {
            qubits.push_back(resourceIndex(call->getArgOperand(1), program.qubits));
          }
          const OperationId operationId{nextOperation++};
          const auto result =
              operation == QuantumOp::MeasureZ
                  ? std::optional{resourceIndex(call->getArgOperand(1), program.results)}
                  : std::nullopt;
          operations.operations.push_back({operationId, {*operation, std::move(qubits), result}});
          body.ir->operations.emplace(operationId, call);
        }
      }
    }
    body.blocks.push_back(std::move(operations));
  }
}
} // namespace qsbit
