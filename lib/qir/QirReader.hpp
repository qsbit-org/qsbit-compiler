#pragma once
#include "model/Program.hpp"
#include <filesystem>
namespace qsbit {
Program readQIR(const std::filesystem::path &);
}
