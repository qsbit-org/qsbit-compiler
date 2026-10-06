#include "qsbit/Compiler.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    std::filesystem::path input, target, output;
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--help" || argument == "-h") {
        std::cout << "Usage: qsbitc INPUT.ll --target TARGET.json -o OUTPUT.elf\n"
                     "Compile supported QIR Base and Adaptive programs; accepts LLVM bitcode.\n"
                     "Emits ELF, lowered LLVM IR, manifest, schedule, and simulator run config.\n";
        return 0;
      }
      if (argument == "--version") {
        std::cout << "qsbitc 0.1.0 (LLVM 21)\n";
        return 0;
      }
      if (argument == "--target" || argument == "-o") {
        if (++i >= argc)
          throw std::runtime_error("missing value for " + argument);
        (argument == "--target" ? target : output) = argv[i];
      } else if (argument.starts_with('-') || !input.empty()) {
        throw std::runtime_error("unexpected argument: " + argument);
      } else {
        input = argument;
      }
    }
    if (input.empty() || target.empty() || output.empty())
      throw std::runtime_error("required: INPUT --target TARGET -o OUTPUT.elf (see --help)");
    if (output.extension() != ".elf")
      throw std::runtime_error("output filename must end in .elf");
    if (std::filesystem::absolute(input).lexically_normal() ==
        std::filesystem::absolute(output).lexically_normal())
      throw std::runtime_error("input and output must differ");
    auto program = qsbit::readQIR(input);
    auto device = qsbit::readTarget(target);
    auto plan = qsbit::schedule(program, device);
    qsbit::emitExecutable(program, device, plan, output, QSBIT_LLD);
    std::cout << "Compiled " << program.entry << " to " << output.string() << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "qsbitc: " << error.what() << '\n';
    return 1;
  }
}
