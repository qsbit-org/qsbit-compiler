#pragma once
#include "target/TargetModel.hpp"
#include <filesystem>
namespace qsbit {
void validateELF(const std::filesystem::path &, const Target &, bool adaptive);
}
