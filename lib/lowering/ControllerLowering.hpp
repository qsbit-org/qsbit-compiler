#pragma once
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include <llvm/IR/Module.h>
namespace qsbit {
std::unique_ptr<llvm::Module> lowerAdaptive(Program &, const Target &, const AdaptiveSchedule &);
void lowerStatic(llvm::Module &, const StaticProgram &, const StaticSchedule &);
} // namespace qsbit
