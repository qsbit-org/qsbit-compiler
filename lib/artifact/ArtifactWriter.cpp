#include "artifact/ArtifactWriter.hpp"
#include "config/TargetReader.hpp"
#include "model/Program.hpp"
#include "schedule/ScheduledProgram.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <qsbit/contracts/executable.hpp>
#include <stdexcept>
#include <utility>
#include <variant>
namespace qsbit {
namespace {
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

ArtifactBundle prepareArtifacts(const Program &program, const TargetInput &input,
                                const Schedule &plan, const std::filesystem::path &output) {
  const auto &target = input.model;
  const bool adaptive = program.adaptive();
  llvm::json::Array outputs;
  llvm::json::Object attributes;
  for (const auto &[key, value] : program.attributes)
    attributes[key] = value;
  std::uint32_t index = 0;
  if (!adaptive) {
    const auto &body = std::get<StaticProgram>(program.body);
    for (const auto &record : body.outputs) {
      llvm::json::Object json{{"kind", record.kind}};
      if (record.label)
        json["label"] = *record.label;
      else
        json["label"] = nullptr;
      if (record.kind == "result") {
        json["result_id"] = record.value;
        json["address"] = contract::abi::Static.output_data + contract::abi::WordBytes * index++;
      } else {
        json["length"] = record.value;
      }
      outputs.push_back(std::move(json));
    }
  }
  auto absolute = std::filesystem::absolute(output);
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
  llvm::json::Object timing{{"schema", contract::abi::Schema}};
  if (adaptive) {
    timing["block_cycles"] = target.blockCycles;
    timing["tcu_period_ns"] = target.tcuPeriod;
    timing["start_ns"] = target.start;
    timing["mode"] = "control-flow";
  } else {
    timing["mode"] = "static";
    timing["events"] = std::move(events);
  }
  llvm::json::Object manifest{
      {"schema", contract::abi::Schema},
      {"compiler_version", "0.1.0"},
      {"isa", std::string(contract::abi::Isa)},
      {"abi", std::string((adaptive ? contract::abi::Adaptive : contract::abi::Static).identifier)},
      {"qir_dialect", program.dialect},
      {"entry", program.entry},
      {"entry_attributes", std::move(attributes)},
      {"target", target.name},
      {"profile", llvm::json::Object(profile)},
      {"elf_sha256", ""},
      {"outputs", std::move(outputs)},
      {"output_word_count", index},
      {"stack_pointer",
       (adaptive ? contract::abi::Adaptive : contract::abi::Static).stack_pointer}};
  if (adaptive) {
    manifest.erase("output_word_count");
    manifest["output_buffer"] =
        llvm::json::Object{{"count_address", contract::abi::Adaptive.output_count},
                           {"data_address", contract::abi::Adaptive.output_data},
                           {"capacity", contract::abi::Adaptive.output_capacity},
                           {"word_bytes", contract::abi::WordBytes}};
  }
  if (!input.decoding.empty())
    manifest["decoding"] = llvm::json::Object(input.decoding);
  llvm::json::Array inspect;
  for (std::uint32_t i = 0; i < index; ++i)
    inspect.push_back(contract::abi::Static.output_data + contract::abi::WordBytes * i);
  if (adaptive)
    inspect.push_back(contract::abi::Adaptive.output_count);
  llvm::json::Object run{{"schema", contract::abi::Schema},
                         {"program", absolute.filename().string()},
                         {"profile", std::move(profile)},
                         {"backend", "mock"},
                         {"inspect", std::move(inspect)},
                         {"summary", absolute.stem().string() + ".summary.json"},
                         {"trace", absolute.stem().string() + ".trace.jsonl"}};
  if (adaptive) {
    run["memory_size"] = contract::abi::Adaptive.memory_size;
    run["memory_dump"] = absolute.stem().string() + ".memory.bin";
  }
  if (!input.decoding.empty())
    run["decoding"] = llvm::json::Object(input.decoding);
  return {std::move(manifest), std::move(run), std::move(timing)};
}
void publishArtifacts(ArtifactBundle bundle, const std::string &digest,
                      const std::filesystem::path &directory, const std::filesystem::path &output) {
  const auto absolute = std::filesystem::absolute(output);
  const auto artifact = [&](const char *suffix) {
    return directory / (absolute.stem().string() + suffix);
  };
  bundle.manifest["elf_sha256"] = digest;
  jsonFile(artifact(".manifest.json"), std::move(bundle.manifest));
  jsonFile(artifact(".run.json"), std::move(bundle.run));
  jsonFile(artifact(".schedule.json"), std::move(bundle.schedule));
  for (const auto *suffix : {".lowered.ll", ".schedule.json", ".manifest.json", ".run.json"})
    std::filesystem::rename(artifact(suffix), absolute.parent_path() / artifact(suffix).filename());
  std::filesystem::rename(artifact(".elf"), absolute);
}
} // namespace qsbit
