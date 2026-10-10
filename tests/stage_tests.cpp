#include "artifact/ArtifactWriter.hpp"
#include "lowering/ControllerLowering.hpp"
#include "qir/AdaptiveIR.hpp"
#include "qir/QirReader.hpp"
#include <iostream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <stdexcept>

namespace {
void check(bool value) {
  if (!value)
    throw std::runtime_error("stage contract failed");
}
qsbit::Target target() {
  qsbit::Target t;
  t.qubits = t.ports = 1;
  t.mappings = {{qsbit::QuantumOp::X, {0}, 0, 1, 20}, {qsbit::QuantumOp::MeasureZ, {0}, 0, 2, 20}};
  return t;
}
std::unique_ptr<qsbit::OwnedModule> lower(const std::filesystem::path &source, bool insert) {
  auto program = qsbit::readQIR(source);
  auto plan = qsbit::schedule(program, target());
  if (insert) {
    auto &ir = *std::get<qsbit::AdaptiveProgram>(program.body).ir;
    auto *call =
        llvm::cast<llvm::Instruction>(static_cast<llvm::Value *>(ir.operations.begin()->second));
    auto *constant = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ir.code->context), 1);
    llvm::BinaryOperator::CreateAdd(constant, constant, "unrelated", call->getIterator());
  }
  return qsbit::lowerProgram(std::move(program), target(), plan);
}
} // namespace
int main(int argc, char **argv) {
  try {
    check(argc == 3);
    const std::string mode = argv[1];
    const auto source = std::filesystem::path(argv[2]) / "examples/qec/feedback.ll";
    if (mode == "owner" || mode == "association") {
      auto result = lower(source, mode == "association");
      check(result->module && &result->module->getContext() == result->context.get());
      check(!llvm::verifyModule(*result->module, &llvm::errs()));
      if (mode == "association") {
        auto program = qsbit::readQIR(source);
        auto plan = qsbit::schedule(program, target());
        auto &ir = *std::get<qsbit::AdaptiveProgram>(program.body).ir;
        llvm::cast<llvm::Instruction>(static_cast<llvm::Value *>(ir.operations.begin()->second))
            ->eraseFromParent();
        bool rejected = false;
        try {
          (void)qsbit::lowerProgram(std::move(program), target(), plan);
        } catch (const std::logic_error &) {
          rejected = true;
        }
        check(rejected);
      }
    } else if (mode == "artifact") {
      qsbit::Program program;
      program.entry = "example";
      program.body.emplace<qsbit::AdaptiveProgram>();
      qsbit::TargetInput input{target(), {}};
      auto bundle =
          qsbit::prepareArtifacts(program, input, qsbit::AdaptiveSchedule{}, "example.elf");
      const auto *output = bundle.manifest.getObject("output_buffer");
      check(output &&
            output->getInteger("count_address") == qsbit::contract::abi::Adaptive.output_count);
      check(output->getInteger("data_address") == qsbit::contract::abi::Adaptive.output_data);
      check(output->getInteger("capacity") == qsbit::contract::abi::Adaptive.output_capacity);
      check(bundle.run.getInteger("memory_size") == qsbit::contract::abi::Adaptive.memory_size);
      check(bundle.manifest.getInteger("stack_pointer") ==
            qsbit::contract::abi::Adaptive.stack_pointer);
    } else {
      throw std::runtime_error("unknown stage test");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
