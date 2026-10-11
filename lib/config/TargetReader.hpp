#pragma once
#include "model/Program.hpp"
#include "target/TargetModel.hpp"
#include <filesystem>
#include <llvm/Support/JSON.h>
namespace qsbit {
struct TargetInput {
  Target model;
  llvm::json::Object decoding;
};
TargetInput readTarget(const std::filesystem::path &, const Program &);
} // namespace qsbit
