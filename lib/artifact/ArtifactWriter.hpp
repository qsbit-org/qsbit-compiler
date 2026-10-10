#pragma once
#include "config/TargetReader.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
namespace qsbit {
void emitExecutable(Program &, const TargetInput &, const Schedule &, const std::filesystem::path &,
                    const std::string &);
llvm::json::Object simulatorProfile(const Target &, bool adaptive);
} // namespace qsbit
