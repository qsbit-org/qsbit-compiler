#pragma once
#include <cstdint>
#include <qsbit/contracts/operations.hpp>
#include <string>
#include <string_view>
#include <vector>
namespace qsbit {
using contract::operation_name;
using contract::QuantumOp;
struct Mapping {
  QuantumOp operation = QuantumOp::X;
  std::vector<std::uint32_t> qubits;
  std::uint32_t port = 0, codeword = 0, duration = 0;
};
struct Target {
  std::uint32_t blockCycles = 1000, decoderBase = 0x40000000;
  bool hasDecoder = false;
  std::string name;
  std::uint32_t qubits = 0, ports = 0;
  std::uint32_t start = 10000, tcuPeriod = 20;
  std::uint32_t timingCapacity = 32, eventCapacity = 32, resultCapacity = 8;
  std::uint32_t stagingCapacity = 16;
  std::vector<Mapping> mappings;
};
const Mapping &mapping(const Target &, QuantumOp, const std::vector<std::uint32_t> &);
} // namespace qsbit
