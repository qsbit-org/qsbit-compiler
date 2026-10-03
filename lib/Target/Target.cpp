#include "qsbit/Compiler.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <set>
#include <stdexcept>

namespace qsbit {
namespace {
[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error("target: " + message);
}
void keys(const llvm::json::Object &object, std::initializer_list<llvm::StringRef> allowed) {
  for (const auto &item : object) {
    bool found = false;
    for (auto key : allowed)
      found |= llvm::StringRef(item.first) == key;
    if (!found)
      fail("unknown key " + item.first.str());
  }
}
std::uint32_t integer(const llvm::json::Object &object, llvm::StringRef key, std::uint32_t low,
                      std::uint32_t high) {
  auto value = object.getInteger(key);
  if (!value || *value < low || *value > high)
    fail("missing or out-of-range " + key.str());
  return static_cast<std::uint32_t>(*value);
}
std::string string(const llvm::json::Object &object, llvm::StringRef key) {
  auto value = object.getString(key);
  if (!value || value->empty())
    fail("missing or empty " + key.str());
  return value->str();
}
} // namespace
Target readTarget(const std::filesystem::path &path) {
  auto buffer = llvm::MemoryBuffer::getFile(path.string());
  if (!buffer)
    fail("cannot read " + path.string());
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed)
    fail(llvm::toString(parsed.takeError()));
  auto *object = parsed->getAsObject();
  if (!object)
    fail("expected an object");
  keys(*object, {"schema", "name", "qubits", "ports", "start_ns", "mappings"});
  integer(*object, "schema", 1, 1);
  Target target;
  target.name = string(*object, "name");
  target.qubits = integer(*object, "qubits", 1, 32);
  const auto ports = integer(*object, "ports", 1, 64);
  target.start = integer(*object, "start_ns", 10000, 100000);
  if (target.start % target.tcuPeriod != 0)
    fail("start_ns must be aligned to the 20 ns TCU clock");
  auto *mappings = object->getArray("mappings");
  if (!mappings || mappings->empty())
    fail("mappings must be a nonempty array");
  llvm::json::Array profileMappings;
  std::set<std::pair<std::uint32_t, std::uint32_t>> codes;
  std::set<std::pair<std::string, std::vector<std::uint32_t>>> operations;
  for (const auto &value : *mappings) {
    auto *spec = value.getAsObject();
    if (!spec)
      fail("mapping must be an object");
    keys(*spec, {"operation", "qubits", "port", "codeword", "duration_ns"});
    Mapping mapping;
    mapping.operation = string(*spec, "operation");
    if (mapping.operation != "h" && mapping.operation != "x" && mapping.operation != "z" &&
        mapping.operation != "cx" && mapping.operation != "measure")
      fail("unsupported operation " + mapping.operation);
    mapping.port = integer(*spec, "port", 0, ports - 1);
    mapping.codeword = integer(*spec, "codeword", 0, 65535);
    mapping.duration = integer(*spec, "duration_ns", 1, 10000);
    auto *qubits = spec->getArray("qubits");
    if (!qubits || qubits->size() != (mapping.operation == "cx" ? 2 : 1))
      fail("incorrect qubit arity");
    llvm::json::Array targets, resources;
    for (const auto &qubit : *qubits) {
      auto number = qubit.getAsInteger();
      if (!number || *number < 0 || *number >= target.qubits)
        fail("qubit is out of range");
      mapping.qubits.push_back(static_cast<std::uint32_t>(*number));
      targets.push_back(*number);
      resources.push_back(llvm::json::Object{{"id", *number}, {"exclusive", true}});
    }
    if (mapping.qubits.size() == 2 && mapping.qubits[0] == mapping.qubits[1])
      fail("CX requires distinct qubits");
    if (!codes.emplace(mapping.port, mapping.codeword).second ||
        !operations.emplace(mapping.operation, mapping.qubits).second)
      fail("duplicate codeword or operation mapping");
    llvm::json::Object action{{"kind", mapping.operation == "measure" ? "acquire" : "gate"},
                              {"operation", mapping.operation},
                              {"port", mapping.port},
                              {"targets", std::move(targets)},
                              {"resources", std::move(resources)},
                              {"delay", 0},
                              {"duration", mapping.duration}};
    if (mapping.operation == "measure")
      action["discriminator_delay"] = 20;
    llvm::json::Array actions;
    actions.push_back(std::move(action));
    profileMappings.push_back(llvm::json::Object{
        {"port", mapping.port}, {"codeword", mapping.codeword}, {"actions", std::move(actions)}});
    target.mappings.push_back(std::move(mapping));
  }
  target.profile = llvm::json::Object{{"schema", 1},
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
                                      {"staging_capacity", 16},
                                      {"result_capacity", target.resultCapacity},
                                      {"ports", ports},
                                      {"qubits", target.qubits},
                                      {"firing_width", 1},
                                      {"seed", 1},
                                      {"fast_feedback", false},
                                      {"two_qubit_gates", llvm::json::Array{}},
                                      {"mappings", std::move(profileMappings)}};
  return target;
}
} // namespace qsbit
