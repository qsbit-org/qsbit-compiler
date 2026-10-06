#pragma once
#include "qsbit/Compiler.hpp"
#include <set>
namespace qsbit {
std::uint32_t resourceIndex(llvm::Value *value, std::uint32_t limit);
std::unique_ptr<llvm::Module> prepareAdaptive(const Program &program, llvm::LLVMContext &context);
const Mapping &mapping(const Target &target, llvm::StringRef name,
                       const std::vector<std::uint32_t> &qubits);
struct OperationTiming {
  std::uint32_t before = 0, after = 0;
};
class BlockSchedule {
public:
  explicit BlockSchedule(const Target &target) : target_(target) {}
  OperationTiming reserve(const Mapping &mapping, bool measurement);
  std::uint32_t finish() const;

private:
  const Target &target_;
  std::uint32_t used_ = 0, commands_ = 0;
  std::set<std::uint32_t> qubits_, ports_;
};
} // namespace qsbit
