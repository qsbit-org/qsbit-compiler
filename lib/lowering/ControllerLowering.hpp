#pragma once
#include "llvm_ir/OwnedModule.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
namespace qsbit {
std::unique_ptr<OwnedModule> lowerProgram(Program, const Target &, const Schedule &);
std::unique_ptr<OwnedModule> lowerAdaptive(Program &, const Target &, const AdaptiveSchedule &);
void lowerStatic(llvm::Module &, const StaticProgram &, const StaticSchedule &);
} // namespace qsbit
