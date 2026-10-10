#pragma once
#include "config/TargetReader.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
namespace qsbit {
struct ArtifactBundle {
  llvm::json::Object manifest, run, schedule;
};
ArtifactBundle prepareArtifacts(const Program &, const TargetInput &, const Schedule &,
                                const std::filesystem::path &);
void publishArtifacts(ArtifactBundle, const std::string &digest,
                      const std::filesystem::path &directory, const std::filesystem::path &output);
llvm::json::Object simulatorProfile(const Target &, bool adaptive);
} // namespace qsbit
