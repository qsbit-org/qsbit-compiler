#pragma once
#include <filesystem>
#include <string>
namespace qsbit {
std::string compileProgram(const std::filesystem::path &input, const std::filesystem::path &target,
                           const std::filesystem::path &output, const std::string &linker);
}
