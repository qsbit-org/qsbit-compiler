#include "qsbit/Compiler.hpp"
#include <limits>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <set>
#include <stdexcept>

namespace qsbit {
namespace {
[[noreturn]] void fail(const std::string &message) { throw std::runtime_error("QIR: " + message); }
std::uint32_t number(llvm::Value *value) {
  auto *integer = llvm::dyn_cast<llvm::ConstantInt>(value);
  if (!integer || integer->getValue().getActiveBits() > 32)
    fail("expected a nonnegative 32-bit static identifier or count");
  return static_cast<std::uint32_t>(integer->getZExtValue());
}
std::uint32_t identifier(llvm::Value *value, std::uint32_t limit) {
  if (!value->getType()->isPointerTy())
    fail("qubit and result operands must be pointers");
  std::uint32_t index = 0;
  if (!llvm::isa<llvm::ConstantPointerNull>(value)) {
    auto *expr = llvm::dyn_cast<llvm::ConstantExpr>(value);
    if (!expr || expr->getOpcode() != llvm::Instruction::IntToPtr)
      fail("dynamic qubit or result identifier is unsupported");
    index = number(expr->getOperand(0));
  }
  if (index >= limit)
    fail("identifier " + std::to_string(index) + " exceeds declared resource count");
  return index;
}
std::uint32_t attributeCount(const llvm::Function &entry, llvm::StringRef standard,
                             llvm::StringRef legacy) {
  auto attribute = entry.getFnAttribute(standard);
  auto other = entry.getFnAttribute(legacy);
  if (!attribute.isValid())
    attribute = other;
  else if (other.isValid() && attribute.getValueAsString() != other.getValueAsString())
    fail("conflicting resource attributes");
  std::uint32_t count = 0;
  if (!attribute.isStringAttribute() || attribute.getValueAsString().getAsInteger(10, count))
    fail("missing or invalid " + standard.str());
  if (count > 64)
    fail("resource count exceeds the supported limit of 64");
  return count;
}
std::optional<std::string> label(llvm::Value *value) {
  if (!value->getType()->isPointerTy())
    fail("output label must be a pointer");
  if (llvm::isa<llvm::ConstantPointerNull>(value))
    return std::nullopt;
  llvm::StringRef text;
  if (!llvm::getConstantStringInfo(value, text))
    fail("output label must be a constant string");
  return text.str();
}
void signature(const llvm::CallInst &call, unsigned arguments) {
  if (!call.getType()->isVoidTy() || call.arg_size() != arguments ||
      call.getFunctionType()->isVarArg() || call.hasOperandBundles() || call.isMustTailCall())
    fail("unsupported call signature or call semantics");
  if (!call.use_empty())
    fail("quantum call result must not be used as a classical value");
}
} // namespace

Program readQIR(const std::filesystem::path &path) {
  llvm::LLVMContext context;
  llvm::SMDiagnostic diagnostic;
  auto module = llvm::parseIRFile(path.string(), diagnostic, context);
  if (!module) {
    std::string error;
    llvm::raw_string_ostream stream(error);
    diagnostic.print("qsbitc", stream);
    fail(error);
  }
  std::string error;
  llvm::raw_string_ostream stream(error);
  if (llvm::verifyModule(*module, &stream))
    fail(error);
  llvm::Function *entry = nullptr;
  for (auto &function : *module) {
    if (function.hasFnAttribute("entry_point")) {
      if (entry)
        fail("multiple entry points are unsupported");
      entry = &function;
    }
    if (!function.isDeclaration() && !function.hasFnAttribute("entry_point"))
      fail("helper function definitions are unsupported");
  }
  if (!entry || entry->isDeclaration())
    fail("expected one defined entry_point");
  if (!entry->arg_empty() || !entry->getReturnType()->isVoidTy() || entry->isVarArg() ||
      entry->size() != 1)
    fail("entry point must be a void, argument-free, single-block function");
  if (!entry->hasFnAttribute("qir_profiles") ||
      entry->getFnAttribute("qir_profiles").getValueAsString() != "base_profile")
    fail("only the static base_profile subset is supported");
  if (!module->getModuleInlineAsm().empty())
    fail("module assembly is unsupported");
  Program program;
  program.entry = entry->getName().str();
  program.qubits = attributeCount(*entry, "required_num_qubits", "requiredQubits");
  program.results = attributeCount(*entry, "required_num_results", "requiredResults");
  for (auto attribute : entry->getAttributes().getFnAttrs())
    if (attribute.isStringAttribute())
      program.attributes[attribute.getKindAsString().str()] = attribute.getValueAsString().str();
  for (const auto *flag : {"dynamic_qubit_management", "dynamic_result_management"}) {
    if (auto *metadata = module->getModuleFlag(flag)) {
      auto *value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(metadata);
      if (!value || !value->isZero())
        fail(std::string(flag) + " must be false");
    }
  }
  if (auto *metadata = module->getModuleFlag("qir_major_version")) {
    auto *major = llvm::mdconst::dyn_extract<llvm::ConstantInt>(metadata);
    auto *minor = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(
        module->getModuleFlag("qir_minor_version"));
    if (!major || !minor || !major->equalsInt(1) || !minor->isZero())
      fail("only QIR 1.0 versioned input is supported");
    program.dialect = "qir-1.0-static-base-subset";
  } else {
    if (!entry->hasFnAttribute("requiredQubits") || module->getModuleFlag("qir_minor_version"))
      fail("missing QIR version; unversioned input requires CUDA-Q resource attributes");
    program.dialect = "cudaq-static-base-unversioned";
  }
  std::set<std::uint32_t> measured, assigned;
  bool measurementPhase = false, outputPhase = false;
  std::vector<std::uint32_t> containers;
  for (auto &instruction : entry->front()) {
    if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
      if (ret->getReturnValue())
        fail("entry point must return void");
      continue;
    }
    auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction);
    if (!call || !call->getCalledFunction() || call->isInlineAsm())
      fail("only direct supported QIR calls and ret void are allowed");
    auto name = call->getCalledFunction()->getName();
    if (name == "__quantum__rt__initialize") {
      signature(*call, 1);
      if (&instruction != &entry->front().front() ||
          !llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(0)))
        fail("initialize must be the first instruction with a null argument");
      continue;
    }
    if (name == "__quantum__rt__array_record_output" ||
        name == "__quantum__rt__tuple_record_output" ||
        name == "__quantum__rt__result_record_output") {
      signature(*call, 2);
      outputPhase = true;
      while (!containers.empty() && containers.back() == 0)
        containers.pop_back();
      if (!containers.empty())
        --containers.back();
      auto outputLabel = label(call->getArgOperand(1));
      if (name == "__quantum__rt__result_record_output") {
        auto id = identifier(call->getArgOperand(0), program.results);
        if (!assigned.contains(id))
          fail("output refers to an unmeasured result");
        program.outputs.push_back({"result", id, outputLabel});
      } else {
        if (!call->getArgOperand(0)->getType()->isIntegerTy(64))
          fail("output container size must be i64");
        auto count = number(call->getArgOperand(0));
        if (count > 64 || containers.size() >= 16)
          fail("output container exceeds supported bounds");
        program.outputs.push_back({name.contains("array") ? "array" : "tuple", count, outputLabel});
        containers.push_back(count);
      }
      if (program.outputs.size() > 128)
        fail("too many output records");
      continue;
    }
    if (outputPhase)
      fail("quantum operations cannot follow output recording");
    Operation op;
    if (name == "__quantum__qis__h__body" || name == "__quantum__qis__x__body" ||
        name == "__quantum__qis__z__body") {
      signature(*call, 1);
      op.name = name == "__quantum__qis__h__body"   ? "h"
                : name == "__quantum__qis__x__body" ? "x"
                                                    : "z";
      op.qubits = {identifier(call->getArgOperand(0), program.qubits)};
    } else if (name == "__quantum__qis__cnot__body" || name == "__quantum__qis__cx__body") {
      signature(*call, 2);
      op.name = "cx";
      op.qubits = {identifier(call->getArgOperand(0), program.qubits),
                   identifier(call->getArgOperand(1), program.qubits)};
      if (op.qubits[0] == op.qubits[1])
        fail("CX requires distinct qubits");
    } else if (name == "__quantum__qis__mz__body") {
      signature(*call, 2);
      measurementPhase = true;
      op.name = "measure";
      auto qubit = identifier(call->getArgOperand(0), program.qubits);
      auto result = identifier(call->getArgOperand(1), program.results);
      if (!measured.insert(qubit).second || !assigned.insert(result).second)
        fail("repeated measurement or result reuse is unsupported");
      op.qubits = {qubit};
      op.result = result;
    } else {
      fail("unsupported call: " + name.str());
    }
    if (measurementPhase && op.name != "measure")
      fail("unitary gates after measurement are unsupported");
    program.operations.push_back(std::move(op));
    if (program.operations.size() > MaxOperations)
      fail("program exceeds the static scheduler limit of 16 operations");
  }
  for (auto remaining : containers)
    if (remaining != 0)
      fail("incomplete output container");
  if (!llvm::isa<llvm::ReturnInst>(entry->front().getTerminator()))
    fail("entry point must terminate with ret void");
  return program;
}
} // namespace qsbit
