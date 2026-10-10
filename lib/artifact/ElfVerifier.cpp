#include "artifact/ElfVerifier.hpp"
#include "target/TargetModel.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <llvm/ADT/StringRef.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <qsbit/contracts/executable.hpp>
#include <qsbit/contracts/isa.hpp>
#include <stdexcept>
#include <string>
namespace qsbit {
std::uint32_t word(llvm::StringRef bytes, std::size_t offset) {
  if (offset + 4 > bytes.size())
    throw std::runtime_error("truncated ELF");
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= std::uint32_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  return value;
}
void validateELF(const std::filesystem::path &path, const Target &target, bool adaptive) {
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
    if (content->size() % 4 != 0 ||
        content->size() >=
            (adaptive ? contract::abi::Adaptive.output_count : contract::abi::Static.output_data))
      throw std::runtime_error("text must contain aligned RV32 instructions below output RAM");
    // Count preload instructions before the first FMR or exit call.
    std::uint32_t preload = 0;
    bool reading = false;
    for (std::size_t offset = 0; offset < content->size(); offset += 4) {
      const auto instruction = word(*content, offset);
      if (adaptive && instruction == 0)
        continue;
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
      case contract::ControlOpcode:
        if (!contract::valid_control_word(instruction) || (instruction >> 25) != 0 ||
            (funct3 != 0 && funct3 != 1 && funct3 != 3) ||
            (funct3 != 3 && ((instruction >> 7) & 31) != 0) ||
            (funct3 != 0 && ((instruction >> 20) & 31) != 0))
          throw std::runtime_error("unsupported quantum instruction in ELF");
        reading |= funct3 == 3;
        break;
      case 0x73:
        if (instruction != 0x73)
          throw std::runtime_error("unsupported system instruction in ELF");
        reading = true;
        break;
      default:
        throw std::runtime_error("unexpected instruction in ELF: " + std::to_string(instruction));
      }
      if (!reading)
        ++preload;
    }
    if (!adaptive && std::uint64_t(preload) * 100 >= target.start)
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

} // namespace qsbit
