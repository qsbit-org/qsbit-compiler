#pragma once

#include <cstdint>
#include <filesystem>
#include <llvm/IR/Module.h>
#include <llvm/Support/JSON.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qsbit {
struct Operation {
  std::string name;
  std::vector<std::uint32_t> qubits;
  std::optional<std::uint32_t> result;
};
struct Output {
  std::string kind;
  std::uint32_t value;
  std::optional<std::string> label;
};
struct Program {
  std::string adaptiveIR;
  std::string entry;
  std::uint32_t qubits = 0, results = 0;
  std::string dialect;
  llvm::json::Object attributes;
  std::vector<Operation> operations;
  std::vector<Output> outputs;
};
struct Mapping {
  std::string operation;
  std::vector<std::uint32_t> qubits;
  std::uint32_t port = 0, codeword = 0, duration = 0;
};
struct Target {
  std::uint32_t blockCycles = 1000, decoderBase = 0x40000000;
  llvm::json::Object decoding;
  std::string name;
  std::uint32_t qubits = 0;
  std::uint32_t start = 10000, tcuPeriod = 20;
  std::uint32_t timingCapacity = 32, eventCapacity = 32, resultCapacity = 8;
  std::uint32_t stagingCapacity = 16;
  std::vector<Mapping> mappings;
  llvm::json::Object profile;
};
struct ScheduledOperation {
  Operation operation;
  Mapping mapping;
  std::uint32_t cycle = 0;
};
struct Schedule {
  std::vector<ScheduledOperation> operations;
};
inline constexpr std::uint32_t OutputAddress = 0x1000;
inline constexpr std::uint32_t MaxOperations = 16;
Program readQIR(const std::filesystem::path &path);
Program readAdaptive(llvm::Module &module);
std::unique_ptr<llvm::Module> lowerAdaptive(const Program &program, const Target &target,
                                            llvm::LLVMContext &context);
Target readTarget(const std::filesystem::path &path);
Schedule schedule(const Program &program, const Target &target);
void emitExecutable(const Program &program, const Target &target, const Schedule &plan,
                    const std::filesystem::path &output, const std::string &linker);
} // namespace qsbit
