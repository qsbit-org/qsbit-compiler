"""Compiler acceptance and rejection contracts, including bitcode and ELF checks."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

compiler, root = Path(sys.argv[1]), Path(sys.argv[2])
source = (root / "examples/bell.ll").read_text()
target = json.loads((root / "targets/sim-default.json").read_text())


def check(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="qsbit-contracts-") as tmp:
    directory = Path(tmp)
    count = 0

    def compile_case(text=source, device=target, error=None):
        global count
        count += 1
        case = directory / str(count)
        case.mkdir()
        qir = case / "program.ll"
        config = case / "target.json"
        elf = case / "program.elf"
        qir.write_text(text)
        config.write_text(json.dumps(device))
        result = subprocess.run([compiler, qir, "--target", config, "-o", elf], capture_output=True, text=True)
        if error:
            check(result.returncode != 0 and error in result.stderr, f"expected {error!r}: {result.stderr}")
            check(not elf.exists(), "failed compilation left an executable")
        else:
            check(result.returncode == 0, result.stderr)
        return elf

    elf = compile_case()
    run_config = json.loads(elf.with_suffix(".run.json").read_text())
    check(run_config["backend"] == "mock", "default measurement backend")
    data = elf.read_bytes()
    check(data[:7] == b"\x7fELF\x01\x01\x01", "ELF encoding")
    check(struct.unpack_from("<HH", data, 16) == (2, 243), "ELF executable machine")
    check(struct.unpack_from("<I", data, 36)[0] == 0, "ELF flags")
    manifest = json.loads(elf.with_suffix(".manifest.json").read_text())
    check(manifest["abi"] == "qsbit-static-v2" and manifest["isa"] == "rv32i-qsbit-v2", "controller ABI")
    check(manifest["profile"]["result_capacity"] == 8, "measurement delivery capacity")
    check(manifest["elf_sha256"] == hashlib.sha256(data).hexdigest(), "ELF digest")
    check(manifest["entry_attributes"]["qir_profiles"] == "base_profile", "entry attributes")
    check([r["result_id"] for r in manifest["outputs"] if r["kind"] == "result"] == [0, 1], "output order")
    plan = json.loads(elf.with_suffix(".schedule.json").read_text())["events"]
    check([event["tick_ns"] for event in plan] == [10020, 10040, 10060, 10100], "scheduled times")
    compile_case(source.replace('"base_profile"', '"adaptive_profile"'), error="base_profile")
    compile_case(source.replace('"qir_profiles"="base_profile"', ''), error="base_profile")
    compile_case(source.replace("__quantum__qis__h__body", "__quantum__qis__reset__body"), error="unsupported call")
    compile_case(source.replace('"requiredQubits"="2"', '"requiredQubits"="1"'), error="exceeds declared")
    compile_case(source.replace('"requiredQubits"="2"', '"requiredQubits"="-1"'), error="required_num_qubits")
    compile_case(source.replace('"requiredResults"="2"', '"requiredResults"="1"'), error="exceeds declared")
    compile_case(source.replace('ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 1 to ptr)',
                                'ptr inttoptr (i64 1 to ptr), ptr null'), error="result reuse")
    compile_case(source.replace('call void @__quantum__rt__array_record_output(i64 2',
                                'call void @__quantum__rt__array_record_output(i64 3'), error="incomplete output")
    compile_case(source.replace('  ret void', '  call void @__quantum__qis__h__body(ptr null)\n  ret void'),
                 error="cannot follow output")
    compile_case(source.replace('  call void @__quantum__rt__array_record_output',
                                '  call void @__quantum__qis__h__body(ptr null)\n  call void @__quantum__rt__array_record_output'),
                 error="after measurement")
    compile_case(source.replace('  call void @__quantum__qis__h__body(ptr null)',
                                '  call void @__quantum__qis__h__body(ptr null)\n' * 17), error="16 operations")
    compile_case(source.replace('  ret void', '  br label %end\nend:\n  ret void'), error="single-block")
    compile_case(source + '\ndefine void @helper() { ret void }\n', error="helper function")
    compile_case(source.replace('  ret void', '  %v = add i32 1, 2\n  ret void'), error="only direct")
    compile_case(device=dict(target, start_ns=1), error="start_ns")
    compile_case(device=dict(target, qubits=33), error="qubits")
    compile_case(device=dict(target, unused=True), error="unknown key")
    compile_case(device=dict(target, mappings=target["mappings"][:-1]), error="no target mapping")
    compile_case(device=dict(target, mappings=target["mappings"] + target["mappings"][:1]), error="duplicate")
    remapped = json.loads(json.dumps(target))
    for mapping in remapped["mappings"]:
        mapping["port"] += 3
    remap_elf = compile_case(device=remapped)
    remap_plan = json.loads(remap_elf.with_suffix(".schedule.json").read_text())["events"]
    check(remap_plan[0]["port"] == 3, "port mapping must not assume port == qubit")
    versioned = source.replace('requiredQubits', 'required_num_qubits').replace('requiredResults', 'required_num_results')
    versioned = versioned.replace('!llvm.module.flags = !{!0}', '!llvm.module.flags = !{!0, !1, !2, !3, !4}')
    versioned += '\n!1 = !{i32 1, !"qir_major_version", i32 1}\n!2 = !{i32 7, !"qir_minor_version", i32 0}\n!3 = !{i32 1, !"dynamic_qubit_management", i1 false}\n!4 = !{i32 1, !"dynamic_result_management", i1 false}\n'
    compile_case(versioned)
    compile_case(versioned.replace('!"qir_major_version", i32 1', '!"qir_major_version", i32 2'), error="QIR 1.0")
    compile_case(versioned.replace('i1 false', 'i1 true'), error="must be false")
    compile_case(versioned.replace('!"qir_major_version", i32 1', '!"qir_major_version", i128 18446744073709551617'), error="QIR 1.0")
    llvm_as = shutil.which("llvm-as-21")
    if not llvm_as:
        raise RuntimeError("llvm-as-21 is required for bitcode tests")
    bc = directory / "bell.bc"
    subprocess.run([llvm_as, root / "examples/bell.ll", "-o", bc], check=True)
    subprocess.run([compiler, bc, "--target", root / "targets/sim-default.json", "-o", directory / "bitcode.elf"], check=True)
    # A rejected input must preserve an existing valid bundle.
    bad = directory / "bad.ll"
    bad.write_text("not LLVM IR")
    result = subprocess.run([compiler, bad, "--target", root / "targets/sim-default.json", "-o", elf], capture_output=True)
    check(result.returncode != 0 and elf.read_bytes() == data, "failed compile overwrote a valid ELF")
    print(f"Passed {count} compilation cases, bitcode, ELF, and failed-output preservation checks")
