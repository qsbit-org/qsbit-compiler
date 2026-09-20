#include "qsbit/Compiler.hpp"
#include <fstream>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <map>
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
std::uint32_t word(llvm::StringRef bytes, std::size_t offset) {
  if (offset + 4 > bytes.size())
    throw std::runtime_error("truncated ELF");
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= std::uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  return value;
}
void validateELF(const std::filesystem::path &path, const Target &target) {
  auto bytes = llvm::MemoryBuffer::getFile(path.string());
  if (!bytes)
    throw std::runtime_error("cannot read linked ELF");
  auto data = (*bytes)->getBuffer();
  if (data.size() < 52 || word(data, 0) != 0x464c457f || data[4] != 1 || data[5] != 1 ||
      word(data, 16) != 0x00f30002 || word(data, 36) != 0 || word(data, 24) != 0)
    throw std::runtime_error("expected little-endian RV32I ELF32 with entry 0 and flags 0");
  auto object = llvm::object::ObjectFile::createObjectFile(path.string());
  if (!object)
    throw std::runtime_error(llvm::toString(object.takeError()));
  bool foundText = false;
  for (const auto &section : object->getBinary()->sections()) {
    if (!section.isText())
      continue;
    foundText = true;
    auto content = section.getContents();
    if (!content)
      throw std::runtime_error(llvm::toString(content.takeError()));
    if (content->size() % 4 != 0 || content->size() >= OutputAddress)
      throw std::runtime_error("text must contain aligned RV32 instructions below output RAM");
    // All accepted programs are straight-line after the startup jump. Count the
    // preload prefix, including spills, through QFLUSH and before the first QREAD.
    std::uint32_t preload = 0;
    bool reading = false;
    for (std::size_t offset = 0; offset < content->size(); offset += 4) {
      const auto instruction = word(*content, offset);
      const auto opcode = instruction & 0x7f;
      const auto funct3 = (instruction >> 12) & 7;
      if ((instruction & 3) != 3)
        throw std::runtime_error("compressed or invalid instruction in ELF");
      switch (opcode) {
      case 0x37:
      case 0x17:
      case 0x6f:
      case 0x67:
      case 0x63:
      case 0x03:
      case 0x23:
      case 0x13:
      case 0x0f:
        break;
      case 0x33:
        if ((instruction >> 25) != 0 && (instruction >> 25) != 0x20)
          throw std::runtime_error("non-RV32I arithmetic in ELF");
        break;
      case 0x0b:
        if ((instruction >> 25) != 0 || funct3 > 4)
          throw std::runtime_error("unsupported quantum instruction in ELF");
        reading |= funct3 == 3 || funct3 == 4;
        break;
      default:
        throw std::runtime_error("unexpected instruction in ELF: " + std::to_string(instruction));
      }
      if (!reading)
        ++preload;
    }
    if (std::uint64_t(preload) * 100 >= target.start)
      throw std::runtime_error("preload deadline budget exceeded; increase target start_ns");
  }
  if (!foundText)
    throw std::runtime_error("ELF has no executable text");
  for (const auto &symbol : object->getBinary()->symbols()) {
    auto flags = symbol.getFlags();
    if (!flags)
      throw std::runtime_error(llvm::toString(flags.takeError()));
    if ((*flags & llvm::object::SymbolRef::SF_Undefined) != 0)
      throw std::runtime_error("ELF contains an unresolved symbol");
  }
}
} // namespace

void emitExecutable(const Program &program, const Target &target, const Schedule &plan,
                    const std::filesystem::path &output, const std::string &linker) {
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
  llvm::LLVMContext context;
  llvm::Module module("qsbit-controller", context);
  module.setTargetTriple(triple);
  module.setDataLayout(machine->createDataLayout());
  module.setModuleInlineAsm(
      ".option norvc\n.option norelax\n.section .text.start,\"ax\",@progbits\n"
      ".globl _start\n.type _start,@function\n_start:\n"
      "lui sp, 16\naddi sp, sp, -16\nj qsbit_entry\n.size _start, .-_start\n");
  llvm::IRBuilder<> builder(context);
  auto *voidType = builder.getVoidTy();
  auto *i32 = builder.getInt32Ty();
  auto *function =
      llvm::Function::Create(llvm::FunctionType::get(voidType, false),
                             llvm::GlobalValue::ExternalLinkage, "qsbit_entry", module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  auto emit = [&](llvm::StringRef assembly, llvm::StringRef constraints, llvm::Type *result,
                  llvm::ArrayRef<llvm::Value *> values) -> llvm::Value * {
    std::vector<llvm::Type *> types;
    for (auto *value : values)
      types.push_back(value->getType());
    auto *type = llvm::FunctionType::get(result, types, false);
    return builder.CreateCall(llvm::InlineAsm::get(type, assembly, constraints, true), values);
  };
  auto constant = [&](std::uint32_t value) { return builder.getInt32(value); };
  std::map<std::uint32_t, llvm::Value *> handles, bits;
  std::uint32_t cursor = 0;
  for (const auto &item : plan.operations) {
    emit(".insn r 0x0b, 1, 0, x0, $0, x0", "r,~{memory}", voidType,
         {constant(item.cycle - cursor)});
    cursor = item.cycle;
    if (item.operation.result) {
      handles[*item.operation.result] =
          emit(".insn r 0x0b, 0, 0, $0, $1, $2", "=r,r,r,~{memory}", i32,
               {constant(item.mapping.port), constant(item.mapping.codeword)});
    } else {
      emit(".insn r 0x0b, 0, 0, x0, $0, $1", "r,r,~{memory}", voidType,
           {constant(item.mapping.port), constant(item.mapping.codeword)});
    }
  }
  if (!plan.operations.empty())
    emit(".insn r 0x0b, 2, 0, x0, x0, x0", "~{memory}", voidType, {});
  // Consume each hardware handle once, even if a result has multiple output records.
  for (const auto &[id, handle] : handles)
    bits[id] = emit(".insn r 0x0b, 3, 0, $0, $1, x0", "=r,r,~{memory}", i32, {handle});
  std::uint32_t index = 0;
  llvm::json::Array outputs;
  for (const auto &record : program.outputs) {
    llvm::json::Object json{{"kind", record.kind}};
    if (record.label)
      json["label"] = *record.label;
    else
      json["label"] = nullptr;
    if (record.kind == "result") {
      const auto address = OutputAddress + 4 * index++;
      auto *pointer = builder.CreateIntToPtr(constant(address), builder.getPtrTy());
      builder.CreateStore(bits.at(record.value), pointer, true);
      json["result_id"] = record.value;
      json["address"] = address;
    } else {
      json["length"] = record.value;
    }
    outputs.push_back(std::move(json));
  }
  emit(".insn r 0x0b, 4, 0, x0, x0, x0", "~{memory}", voidType, {});
  builder.CreateRetVoid();
  if (llvm::verifyModule(module, &llvm::errs()))
    throw std::runtime_error("invalid lowered LLVM IR");

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
  std::error_code code;
  llvm::raw_fd_ostream ir(artifact(".lowered.ll").string(), code);
  if (code)
    throw std::runtime_error(code.message());
  module.print(ir, nullptr);
  ir.close();
  llvm::raw_fd_ostream object(artifact(".o").string(), code);
  if (code)
    throw std::runtime_error(code.message());
  llvm::legacy::PassManager passes;
  if (machine->addPassesToEmitFile(passes, object, nullptr, llvm::CodeGenFileType::ObjectFile))
    throw std::runtime_error("RISC-V object emission is unavailable");
  passes.run(module);
  object.close();
  textFile(artifact(".ld"), "OUTPUT_ARCH(riscv)\nENTRY(_start)\nPHDRS { text PT_LOAD FLAGS(5); }\n"
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
  if (llvm::sys::ExecuteAndWait(linker, refs, std::nullopt, {}, 30, 0, &error) != 0)
    throw std::runtime_error("LLD failed: " + error);
  validateELF(artifact(".elf"), target);
  auto bytes = llvm::MemoryBuffer::getFile(artifact(".elf").string());
  if (!bytes)
    throw std::runtime_error("cannot hash ELF");
  auto digest = llvm::SHA256::hash(llvm::arrayRefFromStringRef((*bytes)->getBuffer()));
  llvm::json::Array events;
  for (const auto &item : plan.operations) {
    llvm::json::Array qubits;
    for (auto q : item.operation.qubits)
      qubits.push_back(q);
    events.push_back(llvm::json::Object{{"operation", item.operation.name},
                                        {"qubits", std::move(qubits)},
                                        {"port", item.mapping.port},
                                        {"codeword", item.mapping.codeword},
                                        {"cycle", item.cycle},
                                        {"tick_ns", target.start + item.cycle * target.tcuPeriod},
                                        {"duration_ns", item.mapping.duration}});
  }
  jsonFile(artifact(".schedule.json"),
           llvm::json::Object{{"schema", 1}, {"events", std::move(events)}});
  jsonFile(artifact(".manifest.json"),
           llvm::json::Object{{"schema", 1},
                              {"compiler_version", "0.1.0"},
                              {"isa", "rv32i-qsbit-v1"},
                              {"abi", "qsbit-static-v1"},
                              {"qir_dialect", program.dialect},
                              {"entry", program.entry},
                              {"entry_attributes", llvm::json::Object(program.attributes)},
                              {"target", target.name},
                              {"profile", llvm::json::Object(target.profile)},
                              {"elf_sha256", llvm::toHex(digest, true)},
                              {"outputs", std::move(outputs)},
                              {"output_word_count", index},
                              {"stack_pointer", 65520}});
  llvm::json::Array inspect;
  for (std::uint32_t i = 0; i < index; ++i)
    inspect.push_back(OutputAddress + 4 * i);
  jsonFile(artifact(".run.json"),
           llvm::json::Object{{"schema", 1},
                              {"program", absolute.filename().string()},
                              {"profile", llvm::json::Object(target.profile)},
                              {"backend", "scripted"},
                              {"inspect", std::move(inspect)},
                              {"summary", absolute.stem().string() + ".summary.json"},
                              {"trace", absolute.stem().string() + ".trace.jsonl"}});
  for (const auto *suffix : {".lowered.ll", ".schedule.json", ".manifest.json", ".run.json"})
    std::filesystem::rename(artifact(suffix), absolute.parent_path() / artifact(suffix).filename());
  std::filesystem::rename(artifact(".elf"), absolute);
}
} // namespace qsbit
