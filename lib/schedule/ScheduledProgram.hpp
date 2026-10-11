#pragma once
#include "model/Program.hpp"
#include "target/TargetModel.hpp"
namespace qsbit {
struct ScheduledOperation {
  Operation operation;
  Mapping mapping;
  std::uint32_t cycle = 0;
};
struct StaticSchedule {
  std::vector<ScheduledOperation> operations;
};
struct OperationTiming {
  std::uint32_t before = 0, after = 0;
};
struct ScheduledCall {
  Mapping mapping;
  OperationTiming timing;
  std::vector<BlockOperation> members;
  bool emit = true;
};
struct ScheduledBlock {
  std::map<OperationId, ScheduledCall> calls;
  std::uint32_t finish = 0;
};
struct GroupMapping {
  Mapping command;
  std::vector<Mapping> actions;
};
struct AdaptiveSchedule {
  std::vector<GroupMapping> mappings;
  std::map<BlockId, ScheduledBlock> blocks;
};
using Schedule = std::variant<StaticSchedule, AdaptiveSchedule>;
StaticSchedule scheduleStatic(const StaticProgram &, const Target &);
Schedule schedule(const Program &, const Target &, bool batchOperations = true);
AdaptiveSchedule scheduleAdaptive(const std::vector<BlockOperations> &, const Target &,
                                  bool batchOperations = true);
} // namespace qsbit
