#include "driver/Compile.hpp"
#include "artifact/ArtifactWriter.hpp"
#include "artifact/ElfVerifier.hpp"
#include "backend/RiscvBackend.hpp"
#include "config/TargetReader.hpp"
#include "lowering/ControllerLowering.hpp"
#include "qir/QirReader.hpp"
#include "schedule/ScheduledProgram.hpp"
#include <filesystem>
#include <fstream>
#include <ios>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <optional>
#include <qsbit/contracts/executable.hpp>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
namespace qsbit {
namespace {
struct Scratch {
  std::filesystem::path path;
  ~Scratch() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
void textFile(const std::filesystem::path &path, llvm::StringRef text) {
  std::ofstream stream(path, std::ios::binary);
  stream << text.str();
  stream.close();
  if (!stream)
    throw std::runtime_error("cannot write " + path.string());
}
} // namespace
std::string compileProgram(const std::filesystem::path &inputPath,
                           const std::filesystem::path &targetPath,
                           const std::filesystem::path &output, const std::string &linker) {
  auto program = readQIR(inputPath);
  auto input = readTarget(targetPath);
  const auto &target = input.model;
  auto plan = schedule(program, target);
  auto bundle = prepareArtifacts(program, input, plan, output);
  const auto entry = program.entry;
  const bool adaptive = program.adaptive();
  auto owner = lowerProgram(std::move(program), target, plan);
  auto &module = *owner->module;
  auto absolute = std::filesystem::absolute(output);
  std::filesystem::create_directories(absolute.parent_path());
  llvm::SmallString<128> directory;
  if (auto code = llvm::sys::fs::createUniqueDirectory(
          (absolute.parent_path() / ".qsbit-build").string(), directory))
    throw std::runtime_error(code.message());
  Scratch scratch{std::filesystem::path(directory.str().str())};
  auto artifact = [&](llvm::StringRef suffix) {
    return scratch.path / (absolute.stem().string() + suffix.str());
  };
  emitObject(module, adaptive, artifact(".lowered.ll"), artifact(".o"));
  textFile(artifact(".ld"),
           adaptive ? "OUTPUT_ARCH(riscv)\nENTRY(_start)\nPHDRS { text PT_LOAD FLAGS(5); data "
                      "PT_LOAD FLAGS(6); }\n"
                      "SECTIONS { . = 0; .text : { *(.text.start) *(.text*) } :text\n"
                      ". = " +
                          std::to_string(contract::abi::Adaptive.data_start) +
                          "; .data : { *(.data*) *(.sdata*) *(.rodata*) } :data\n"
                          ".bss : { *(.bss*) *(.sbss*) *(COMMON) } :data\n"
                          "ASSERT(. < " +
                          std::to_string(contract::abi::Adaptive.stack_bottom) +
                          ", \"data overlaps stack\")\n"
                          "/DISCARD/ : { *(.eh_frame*) *(.comment) } }\n"
                    : "OUTPUT_ARCH(riscv)\nENTRY(_start)\nPHDRS { text PT_LOAD FLAGS(5); }\n"
                      "SECTIONS { . = 0; .text : { *(.text.start) *(.text*) } :text\n"
                      "/DISCARD/ : { *(.eh_frame*) *(.comment) } }\n");
  std::vector<std::string> arguments{linker,
                                     "-m",
                                     "elf32lriscv",
                                     "--no-relax",
                                     "-T",
                                     artifact(".ld").string(),
                                     artifact(".o").string(),
                                     "-o",
                                     artifact(".elf").string()};
  std::vector<llvm::StringRef> refs(arguments.begin(), arguments.end());
  std::string error;
  if (llvm::sys::ExecuteAndWait(linker, refs, std::nullopt, {}, 30, 0, &error) != 0)
    throw std::runtime_error("LLD failed: " + error);
  validateELF(artifact(".elf"), target, adaptive);
  auto bytes = llvm::MemoryBuffer::getFile(artifact(".elf").string());
  if (!bytes)
    throw std::runtime_error("cannot hash ELF");
  auto digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef((*bytes)->getBuffer()));
  publishArtifacts(std::move(bundle), llvm::toHex(digest, true), scratch.path, output);
  return entry;
}
} // namespace qsbit
