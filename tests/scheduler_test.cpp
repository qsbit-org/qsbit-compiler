#include "schedule/BlockScheduler.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value) {
  if (!value)
    throw std::runtime_error("scheduler assertion failed");
}
template <typename F> void rejects(F operation) {
  try {
    operation();
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("expected rejection");
}
} // namespace

int main() {
  using namespace qsbit;
  try {
    Target target;
    target.qubits = target.ports = 2;
    target.blockCycles = 10;
    target.mappings = {{QuantumOp::H, {0}, 0, 1, 20},
                       {QuantumOp::X, {0}, 0, 2, 40},
                       {QuantumOp::X, {1}, 1, 2, 60},
                       {QuantumOp::MeasureZ, {0}, 0, 3, 40}};
    Program program;
    program.qubits = 2;
    auto &body = std::get<StaticProgram>(program.body);
    body.operations = {{QuantumOp::H, {0}, {}}, {QuantumOp::X, {1}, {}}};
    auto plan = schedule(program, target);
    const auto &staticPlan = std::get<StaticSchedule>(plan);
    check(staticPlan.operations.at(0).cycle == 1);
    check(staticPlan.operations.at(1).cycle == 2);
    body.operations.push_back({QuantumOp::Z, {0}, {}});
    rejects([&] { (void)schedule(program, target); });
    body.operations.pop_back();
    target.timingCapacity = 1;
    rejects([&] { (void)schedule(program, target); });

    std::vector<BlockOperations> blocks{{{0, {QuantumOp::H, {0}, {}}},
                                         {2, {QuantumOp::X, {1}, {}}},
                                         {3, {QuantumOp::Reset, {0}, {}}}}};
    auto adaptive = scheduleAdaptive(blocks, target);
    const auto &block = adaptive.blocks.at(0);
    check(block.calls.at(0).timing.before == 0);
    check(block.calls.at(2).timing.before == 0);
    check(block.calls.at(3).timing.before == 3);
    check(block.calls.at(3).timing.after == 10);
    check(block.calls.at(3).reset->operation == QuantumOp::X);
    check(block.calls.at(3).resetWait == 10 && block.finish == 10);
    target.stagingCapacity = 1;
    auto limited = scheduleAdaptive(blocks, target);
    check(limited.blocks.at(0).calls.at(2).timing.before == 1);
    check(limited.blocks.at(0).calls.at(3).timing.before == 3);
    std::cout << "PASS scheduler\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
