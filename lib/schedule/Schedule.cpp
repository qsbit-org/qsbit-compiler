#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <stdexcept>
namespace qsbit {
Schedule schedule(const Program &program, const Target &target) {
  if (program.qubits > target.qubits)
    throw std::runtime_error("schedule: target has too few qubits");
  if (program.adaptive())
    return scheduleAdaptive(std::get<AdaptiveProgram>(program.body).blocks, target);
  return scheduleStatic(std::get<StaticProgram>(program.body), target);
}
} // namespace qsbit
