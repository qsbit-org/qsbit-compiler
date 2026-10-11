#include "config/TargetReader.hpp"
#include "model/Program.hpp"
#include "target/TargetModel.hpp"
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <qsbit/contracts/executable.hpp>
#include <qsbit/contracts/operations.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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
// Resolve ideal targets from the validated program, before normal target validation.
void expandIdeal(llvm::json::Object &object, const Program &program) {
  auto *ideal = object.getObject("ideal");
  if (!ideal)
    fail("ideal must be an object");
  if (object.get("qubits") || object.get("ports") || object.get("mappings"))
    fail("ideal cannot be combined with qubits, ports, or mappings");
  keys(*ideal, {"max_qubits", "gate_duration_ns", "measurement_duration_ns", "reset_duration_ns"});
  const auto limit = integer(*ideal, "max_qubits", 1, 32);
  if (program.qubits == 0 || program.qubits > limit)
    fail("program exceeds ideal target qubit allocation");
  const auto gate = integer(*ideal, "gate_duration_ns", 1, 10000);
  const auto measurement = integer(*ideal, "measurement_duration_ns", 1, 10000);
  const auto reset = integer(*ideal, "reset_duration_ns", 1, 10000);
  std::set<std::pair<QuantumOp, std::vector<std::uint32_t>>> operations;
  const auto collect = [&](const Operation &operation) {
    operations.emplace(operation.name, operation.qubits);
  };
  if (const auto *body = std::get_if<StaticProgram>(&program.body)) {
    for (const auto &operation : body->operations)
      collect(operation);
  } else {
    for (const auto &block : std::get<AdaptiveProgram>(program.body).blocks)
      for (const auto &operation : block.operations)
        collect(operation.operation);
  }
  llvm::json::Array mappings;
  std::uint32_t codeword = 0;
  for (const auto &[operation, qubits] : operations) {
    if (qubits.empty())
      fail("ideal mapping requires static qubit operands");
    llvm::json::Array operands;
    for (auto qubit : qubits)
      operands.push_back(qubit);
    mappings.push_back(
        llvm::json::Object{{"operation", std::string(operation_name(operation))},
                           {"qubits", std::move(operands)},
                           {"port", qubits.front()},
                           {"codeword", ++codeword},
                           {"duration_ns", operation == QuantumOp::MeasureZ ? measurement
                                           : operation == QuantumOp::Reset  ? reset
                                                                            : gate}});
  }
  object.erase("ideal");
  object["qubits"] = program.qubits;
  object["ports"] = program.qubits;
  object["mappings"] = std::move(mappings);
}
} // namespace
TargetInput readTarget(const std::filesystem::path &path, const Program &program) {
  auto buffer = llvm::MemoryBuffer::getFile(path.string());
  if (!buffer)
    fail("cannot read " + path.string());
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed)
    fail(llvm::toString(parsed.takeError()));
  auto *object = parsed->getAsObject();
  if (!object)
    fail("expected an object");
  keys(*object, {"schema", "name", "qubits", "ports", "start_ns", "mappings", "block_cycles",
                 "decoding", "cpu_period_ns", "tcu_period_ns", "ideal"});
  if (object->get("ideal"))
    expandIdeal(*object, program);
  integer(*object, "schema", 1, 1);
  TargetInput input;
  auto &target = input.model;
  target.name = string(*object, "name");
  if (object->get("block_cycles"))
    target.blockCycles = integer(*object, "block_cycles", 1, 1000000);
  if (auto *decoding = object->getObject("decoding")) {
    input.decoding = llvm::json::Object(*decoding);
    target.hasDecoder = true;
    target.decoderBase = integer(*decoding, "mmio_base", 0, 0xffffffe0);
    if (target.decoderBase % 4 || target.decoderBase < contract::abi::Adaptive.memory_size)
      fail("decoder MMIO must be aligned and outside program RAM");
  } else if (object->get("decoding"))
    fail("decoding must be an object");
  target.qubits = integer(*object, "qubits", 1, 32);
  target.ports = integer(*object, "ports", 1, 64);
  if (object->get("cpu_period_ns"))
    target.cpuPeriod = integer(*object, "cpu_period_ns", 1, 1000000);
  if (object->get("tcu_period_ns"))
    target.tcuPeriod = integer(*object, "tcu_period_ns", 1, 1000000);
  target.start = integer(*object, "start_ns", 0, std::numeric_limits<std::uint32_t>::max());
  if (target.start % target.tcuPeriod != 0)
    fail("start_ns must be aligned to the configured TCU clock");
  auto *mappings = object->getArray("mappings");
  if (!mappings || mappings->empty())
    fail("mappings must be a nonempty array");
  std::set<std::pair<std::uint32_t, std::uint32_t>> codes;
  std::set<std::pair<QuantumOp, std::vector<std::uint32_t>>> operations;
  for (const auto &value : *mappings) {
    auto *spec = value.getAsObject();
    if (!spec)
      fail("mapping must be an object");
    keys(*spec, {"operation", "qubits", "port", "codeword", "duration_ns"});
    Mapping mapping;
    const auto name = string(*spec, "operation");
    const auto op = contract::parse_operation(name);
    if (!op)
      fail("unsupported operation " + name);
    mapping.operation = *op;
    mapping.port = integer(*spec, "port", 0, target.ports - 1);
    mapping.codeword = integer(*spec, "codeword", 0, 65535);
    mapping.duration = integer(*spec, "duration_ns", 1, 10000);
    auto *qubits = spec->getArray("qubits");
    if (!qubits || qubits->size() != (mapping.operation == QuantumOp::Cx ? 2 : 1))
      fail("incorrect qubit arity");
    for (const auto &qubit : *qubits) {
      auto number = qubit.getAsInteger();
      if (!number || *number < 0 || *number >= target.qubits)
        fail("qubit is out of range");
      mapping.qubits.push_back(static_cast<std::uint32_t>(*number));
    }
    if (mapping.qubits.size() == 2 && mapping.qubits[0] == mapping.qubits[1])
      fail("CX requires distinct qubits");
    if (!codes.emplace(mapping.port, mapping.codeword).second ||
        !operations.emplace(mapping.operation, mapping.qubits).second)
      fail("duplicate codeword or operation mapping");
    target.mappings.push_back(std::move(mapping));
  }
  return input;
}
} // namespace qsbit
