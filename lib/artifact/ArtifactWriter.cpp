#include "artifact/ArtifactWriter.hpp"
#include "artifact/ElfVerifier.hpp"
#include "backend/RiscvBackend.hpp"
#include "lowering/ControllerLowering.hpp"
#include "qir/AdaptiveIR.hpp"
#include <fstream>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <stdexcept>

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
void jsonFile(const std::filesystem::path &path, llvm::json::Object object) {
  textFile(path, llvm::formatv("{0:2}\n", llvm::json::Value(std::move(object))).str());
}
} // namespace

void emitExecutable(Program &program, const TargetInput &input, const Schedule &plan,
                    const std::filesystem::path &output, const std::string &linker) {
  const auto &target = input.model;
  llvm::LLVMContext context;
  const bool adaptive = program.adaptive();
  auto moduleOwner = adaptive ? lowerAdaptive(program, target, std::get<AdaptiveSchedule>(plan))
                              : std::make_unique<llvm::Module>("qsbit-controller", context);
  auto &module = *moduleOwner;
  llvm::json::Array outputs;
  llvm::json::Object attributes;
  for (const auto &[key, value] : program.attributes)
    attributes[key] = value;
  std::uint32_t index = 0;
  if (!adaptive) {
    const auto &body = std::get<StaticProgram>(program.body);
    lowerStatic(module, body, std::get<StaticSchedule>(plan));
    for (const auto &record : body.outputs) {
      llvm::json::Object json{{"kind", record.kind}};
      if (record.label)
        json["label"] = *record.label;
      else
        json["label"] = nullptr;
      if (record.kind == "result") {
        json["result_id"] = record.value;
        json["address"] = OutputAddress + 4 * index++;
      } else {
        json["length"] = record.value;
      }
      outputs.push_back(std::move(json));
    }
  }
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
                      ". = 0x20000; .data : { *(.data*) *(.sdata*) *(.rodata*) } :data\n"
                      ".bss : { *(.bss*) *(.sbss*) *(COMMON) } :data\n"
                      "ASSERT(. < 0xe0000, \"data overlaps stack\")\n"
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
  llvm::json::Array events;
  if (const auto *staticPlan = std::get_if<StaticSchedule>(&plan))
    for (const auto &item : staticPlan->operations) {
      llvm::json::Array qubits;
      for (auto q : item.operation.qubits)
        qubits.push_back(q);
      events.push_back(
          llvm::json::Object{{"operation", std::string(operation_name(item.operation.name))},
                             {"qubits", std::move(qubits)},
                             {"port", item.mapping.port},
                             {"codeword", item.mapping.codeword},
                             {"cycle", item.cycle},
                             {"tick_ns", target.start + item.cycle * target.tcuPeriod},
                             {"duration_ns", item.mapping.duration}});
    }
  auto profile = simulatorProfile(target, adaptive);
  llvm::json::Object timing{{"schema", 1}};
  if (adaptive) {
    timing["block_cycles"] = target.blockCycles;
    timing["tcu_period_ns"] = target.tcuPeriod;
    timing["start_ns"] = target.start;
    timing["mode"] = "control-flow";
  } else {
    timing["mode"] = "static";
    timing["events"] = std::move(events);
  }
  jsonFile(artifact(".schedule.json"), std::move(timing));
  llvm::json::Object manifest{{"schema", 1},
                              {"compiler_version", "0.1.0"},
                              {"isa", "rv32i-qsbit-v2"},
                              {"abi", adaptive ? "qsbit-adaptive-v1" : "qsbit-static-v2"},
                              {"qir_dialect", program.dialect},
                              {"entry", program.entry},
                              {"entry_attributes", std::move(attributes)},
                              {"target", target.name},
                              {"profile", llvm::json::Object(profile)},
                              {"elf_sha256", llvm::toHex(digest, true)},
                              {"outputs", std::move(outputs)},
                              {"output_word_count", index},
                              {"stack_pointer", adaptive ? 1048560 : 65520}};
  if (adaptive) {
    manifest.erase("output_word_count");
    manifest["output_buffer"] = llvm::json::Object{{"count_address", 0x10000},
                                                   {"data_address", 0x10004},
                                                   {"capacity", 16383},
                                                   {"word_bytes", 4}};
  }
  if (!input.decoding.empty())
    manifest["decoding"] = llvm::json::Object(input.decoding);
  jsonFile(artifact(".manifest.json"), std::move(manifest));
  llvm::json::Array inspect;
  for (std::uint32_t i = 0; i < index; ++i)
    inspect.push_back(OutputAddress + 4 * i);
  if (adaptive)
    inspect.push_back(0x10000);
  llvm::json::Object run{{"schema", 1},
                         {"program", absolute.filename().string()},
                         {"profile", std::move(profile)},
                         {"backend", "mock"},
                         {"inspect", std::move(inspect)},
                         {"summary", absolute.stem().string() + ".summary.json"},
                         {"trace", absolute.stem().string() + ".trace.jsonl"}};
  if (adaptive) {
    run["memory_size"] = 1048576;
    run["memory_dump"] = absolute.stem().string() + ".memory.bin";
  }
  if (!input.decoding.empty())
    run["decoding"] = llvm::json::Object(input.decoding);
  jsonFile(artifact(".run.json"), std::move(run));
  for (const auto *suffix : {".lowered.ll", ".schedule.json", ".manifest.json", ".run.json"})
    std::filesystem::rename(artifact(suffix), absolute.parent_path() / artifact(suffix).filename());
  std::filesystem::rename(artifact(".elf"), absolute);
}
} // namespace qsbit
