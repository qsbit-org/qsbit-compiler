#pragma once
#include <filesystem>
namespace qsbit {
void validateELF(const std::filesystem::path &, bool adaptive);
}
