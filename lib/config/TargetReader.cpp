#include "config/TargetReader.hpp"
#include "target/TargetModel.hpp"
#include <cstdint>
#include <filesystem>
#include <initializer_list>
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
} // namespace
TargetInput readTarget(const std::filesystem::path &path) {
  auto buffer = llvm::MemoryBuffer::getFile(path.string());
  if (!buffer)
    fail("cannot read " + path.string());
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed)
    fail(llvm::toString(parsed.takeError()));
  auto *object = parsed->getAsObject();
  if (!object)
    fail("expected an object");
  keys(*object,
       {"schema", "name", "qubits", "ports", "start_ns", "mappings", "block_cycles", "decoding"});
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
  target.start = integer(*object, "start_ns", 10000, 100000);
  if (target.start % target.tcuPeriod != 0)
    fail("start_ns must be aligned to the 20 ns TCU clock");
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
    if (!op || *op == QuantumOp::Reset)
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
