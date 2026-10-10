#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <qsbit/contracts/executable.hpp>
#include <qsbit/contracts/operations.hpp>
#include <string>
#include <variant>
#include <vector>

namespace qsbit {
using contract::operation_name;
using contract::QuantumOp;
struct Operation {
  QuantumOp name = QuantumOp::X;
  std::vector<std::uint32_t> qubits;
  std::optional<std::uint32_t> result;
};
struct Output {
  std::string kind;
  std::uint32_t value;
  std::optional<std::string> label;
};
struct StaticProgram {
  std::vector<Operation> operations;
  std::vector<Output> outputs;
};
struct BlockId {
  std::uint32_t value;
  auto operator<=>(const BlockId &) const = default;
};
struct OperationId {
  std::uint32_t value;
  auto operator<=>(const OperationId &) const = default;
};
struct BlockOperation {
  OperationId id;
  Operation operation;
};
struct BlockOperations {
  BlockId id;
  std::vector<BlockOperation> operations;
};
struct AdaptiveIR;
struct AdaptiveProgram {
  std::unique_ptr<AdaptiveIR, void (*)(AdaptiveIR *)> ir{nullptr, nullptr};
  std::vector<BlockOperations> blocks;
};
struct Program {
  std::string entry;
  std::uint32_t qubits = 0, results = 0;
  std::string dialect;
  std::map<std::string, std::string> attributes;
  std::variant<StaticProgram, AdaptiveProgram> body;
  bool adaptive() const { return std::holds_alternative<AdaptiveProgram>(body); }
};
inline constexpr std::uint32_t MaxOperations = 16;
} // namespace qsbit
