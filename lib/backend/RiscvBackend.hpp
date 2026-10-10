#pragma once
#include <filesystem>
namespace llvm {
class Module;
}
namespace qsbit {
void emitObject(llvm::Module &, bool adaptive, const std::filesystem::path &irPath,
                const std::filesystem::path &objectPath);
}
