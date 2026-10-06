#include "qsbit/Adaptive.hpp"
#include <algorithm>
namespace qsbit {
OperationTiming BlockSchedule::reserve(const Mapping &mapping, bool measurement) {
  OperationTiming timing;
  if (commands_ == target_.stagingCapacity || ports_.contains(mapping.port) ||
      std::any_of(mapping.qubits.begin(), mapping.qubits.end(),
                  [&](auto q) { return qubits_.contains(q); })) {
    timing.before = used_;
    used_ = 0;
    commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  ports_.insert(mapping.port);
  ++commands_;
  qubits_.insert(mapping.qubits.begin(), mapping.qubits.end());
  used_ = std::max(
      used_, static_cast<std::uint32_t>((std::uint64_t(mapping.duration) + target_.tcuPeriod - 1) /
                                        target_.tcuPeriod));
  if (measurement) {
    timing.after = std::max(used_, target_.blockCycles);
    used_ = 0;
    commands_ = 0;
    qubits_.clear();
    ports_.clear();
  }
  return timing;
}
std::uint32_t BlockSchedule::finish() const { return std::max(used_, target_.blockCycles); }
} // namespace qsbit
