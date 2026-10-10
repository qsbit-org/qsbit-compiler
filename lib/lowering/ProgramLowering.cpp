#include "llvm_ir/OwnedModule.hpp"
#include "lowering/ControllerLowering.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include "target/TargetModel.hpp"
#include <llvm/IR/Module.h>
#include <memory>
namespace qsbit {
std::unique_ptr<OwnedModule> lowerProgram(Program program, const Target &target,
                                          const Schedule &plan) {
  if (program.adaptive())
    return lowerAdaptive(program, target, std::get<AdaptiveSchedule>(plan));
  auto owner = std::make_unique<OwnedModule>();
  owner->module = std::make_unique<llvm::Module>("qsbit-controller", *owner->context);
  lowerStatic(*owner->module, std::get<StaticProgram>(program.body),
              std::get<StaticSchedule>(plan));
  return owner;
}
} // namespace qsbit
