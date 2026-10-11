#pragma once
#include "schedule/ScheduledProgram.hpp"
#include <set>
#include <span>
namespace qsbit {
class BlockSchedule {
public:
  explicit BlockSchedule(const Target &target) : target_(target) {}
  OperationTiming reserve(std::span<const Mapping> mappings, bool measurement);
  std::uint32_t finish() const;

private:
  const Target &target_;
  std::uint32_t used_ = 0, commands_ = 0;
  std::set<std::uint32_t> qubits_, ports_;
};
} // namespace qsbit
