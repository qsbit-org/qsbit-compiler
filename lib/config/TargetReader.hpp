#pragma once
#include "target/TargetModel.hpp"
#include <filesystem>
#include <llvm/Support/JSON.h>
namespace qsbit {
struct TargetInput {
  Target model;
  llvm::json::Object decoding;
};
TargetInput readTarget(const std::filesystem::path &);
} // namespace qsbit
