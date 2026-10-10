#pragma once
#include <cstdint>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instructions.h>
namespace qsbit {
bool isDecoderCall(llvm::StringRef name);
void validateAdaptiveCall(const llvm::CallInst &, std::uint32_t qubits, std::uint32_t results);
} // namespace qsbit
