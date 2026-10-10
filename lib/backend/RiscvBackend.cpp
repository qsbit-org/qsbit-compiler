#include "backend/RiscvBackend.hpp"
#include <cstdint>
#include <filesystem>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <memory>
#include <qsbit/contracts/executable.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
namespace qsbit {
void emitObject(llvm::Module &module, bool adaptive, const std::filesystem::path &irPath,
                const std::filesystem::path &objectPath) {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();
  const llvm::Triple triple("riscv32-unknown-unknown-elf");
  std::string error;
  const auto *backend = llvm::TargetRegistry::lookupTarget(triple.str(), error);
  if (!backend)
    throw std::runtime_error(error);
  llvm::TargetOptions options;
  options.MCOptions.ABIName = "ilp32";
  auto machine = std::unique_ptr<llvm::TargetMachine>(backend->createTargetMachine(
      triple, "generic-rv32", "+i,-m,-a,-f,-d,-c,-relax", options, llvm::Reloc::Static));
  if (!machine)
    throw std::runtime_error("cannot construct RISC-V target machine");

  module.setTargetTriple(triple);
  module.setDataLayout(machine->createDataLayout());
  const auto &layout = adaptive ? contract::abi::Adaptive : contract::abi::Static;
  module.setModuleInlineAsm(
      std::string(".option norvc\n.option norelax\n.section .text.start,\"ax\",@progbits\n"
                  ".globl _start\n.type _start,@function\n_start:\n") +
      "lui sp, " + std::to_string(layout.memory_size >> 12) + "\naddi sp, sp, " +
      std::to_string(std::int64_t(layout.stack_pointer) - layout.memory_size) +
      "\nj qsbit_entry\n.size _start, .-_start\n");

  if (llvm::verifyModule(module, &llvm::errs()))
    throw std::runtime_error("invalid lowered LLVM IR");
  std::error_code code;
  llvm::raw_fd_ostream ir(irPath.string(), code);
  if (code)
    throw std::runtime_error(code.message());
  module.print(ir, nullptr);
  ir.close();
  llvm::raw_fd_ostream object(objectPath.string(), code);
  if (code)
    throw std::runtime_error(code.message());
  llvm::legacy::PassManager passes;
  if (machine->addPassesToEmitFile(passes, object, nullptr, llvm::CodeGenFileType::ObjectFile))
    throw std::runtime_error("RISC-V object emission is unavailable");
  passes.run(module);
  object.close();
}
} // namespace qsbit
