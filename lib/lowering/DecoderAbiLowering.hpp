#pragma once
#include "target/TargetModel.hpp"
namespace llvm {
class Module;
}
namespace qsbit {
void lowerDecoderABI(llvm::Module &, const Target &);
}
