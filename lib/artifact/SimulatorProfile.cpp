#include "artifact/ArtifactWriter.hpp"
#include "model/Program.hpp"
#include "target/TargetModel.hpp"
#include <llvm/Support/JSON.h>
#include <string>
#include <utility>
namespace qsbit {
llvm::json::Object simulatorProfile(const Target &target, bool adaptive) {
  llvm::json::Array profileMappings;
  for (const auto &mapping : target.mappings) {
    llvm::json::Array targets, resources;
    for (auto q : mapping.qubits) {
      targets.push_back(q);
      resources.push_back(llvm::json::Object{{"id", q}, {"exclusive", true}});
    }
    llvm::json::Object action{
        {"kind", mapping.operation == QuantumOp::MeasureZ ? "acquire" : "gate"},
        {"operation", std::string(operation_name(mapping.operation))},
        {"port", mapping.port},
        {"targets", std::move(targets)},
        {"resources", std::move(resources)},
        {"delay", 0},
        {"duration", mapping.duration}};
    if (mapping.operation == QuantumOp::MeasureZ)
      action["discriminator_delay"] = 20;
    llvm::json::Array actions;
    actions.push_back(std::move(action));
    profileMappings.push_back(llvm::json::Object{
        {"port", mapping.port}, {"codeword", mapping.codeword}, {"actions", std::move(actions)}});
  }
  auto profile = llvm::json::Object{{"schema", 1},
                                    {"cpu", llvm::json::Object{{"period", 5}, {"phase", 0}}},
                                    {"tcu", llvm::json::Object{{"period", 20}, {"phase", 0}}},
                                    {"start", target.start},
                                    {"watchdog", 1000000},
                                    {"memory_latency", 1},
                                    {"command_latency", 1},
                                    {"reply_latency", 1},
                                    {"cpu_result_latency", 1},
                                    {"fast_result_latency", 2},
                                    {"timing_capacity", target.timingCapacity},
                                    {"event_capacity", target.eventCapacity},
                                    {"staging_capacity", target.stagingCapacity},
                                    {"result_capacity", target.resultCapacity},
                                    {"ports", target.ports},
                                    {"qubits", target.qubits},
                                    {"firing_width", 1},
                                    {"seed", 1},
                                    {"fast_feedback", false},
                                    {"two_qubit_gates", llvm::json::Array{}},
                                    {"mappings", std::move(profileMappings)}};

  if (adaptive) {
    profile["firing_width"] = target.ports;
    profile["watchdog"] = 100000000;
  }
  return profile;
}
} // namespace qsbit
