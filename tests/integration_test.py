"""Validate instruction timing, result ownership, and Bell state semantics."""
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

compiler, root, simulator = map(Path, sys.argv[1:4])
backend = sys.argv[4]
spec = importlib.util.spec_from_file_location("qsbit_runner", root / "python/qsbit_compiler/runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix="qsbit-integration-") as temporary:
    directory = Path(temporary)
    elf = directory / "bell.elf"
    subprocess.run([compiler, root / "examples/bell.ll", "--target", root / "targets/sim-default.json", "-o", elf], check=True)
    shots = 16 if backend == "aer" else 1
    result, _ = runner.run(elf, simulator, backend, shots, directory / "runs")
    events = [json.loads(line) for line in (directory / "runs/shot-0000.trace.jsonl").read_text().splitlines()]
    starts = [event for event in events if event["kind"] == "OperationStart"]
    plan = json.loads(elf.with_suffix(".schedule.json").read_text())["events"]
    check([(e["operation"], e["tick"]) for e in starts] ==
          [(e["operation"], e["tick_ns"]) for e in plan], "physical action times differ from the compiled schedule")
    reads = [e for e in events if e["kind"] == "InstructionRetired" and e["word"] & 0x707F == 0x300B]
    check(len(reads) == 2, "expected one FMR per measured qubit")
    check([(e["word"] >> 15) & 31 for e in reads] == [0, 1], "FMR must address qubit registers")
    codewords = [e for e in events if e["kind"] == "InstructionRetired" and e["word"] & 0x707F == 0x000B]
    check(len(codewords) == len(plan) and all((e["word"] >> 7) & 31 == 0 for e in codewords),
          "cw must not write a GPR")
    if backend == "aer":
        check(set(result["counts"]) == {"00", "11"}, f"Bell correlation failed: {result}")
        # Verify the coherent state before measurement; correlation alone also holds for |00>.
        source = (root / "examples/bell.ll").read_text()
        coherent = '\n'.join(line for line in source.splitlines()
                             if not (line.strip().startswith('call') and
                                     ('__quantum__qis__mz__body' in line or 'record_output' in line)))
        state_qir = directory / "coherent.ll"
        state_qir.write_text(coherent)
        state_elf = directory / "coherent.elf"
        subprocess.run([compiler, state_qir, "--target", root / "targets/sim-default.json", "-o", state_elf], check=True)
        _, summaries = runner.run(state_elf, simulator, "aer", 1, directory / "state")
        amplitudes = [complex(*pair) for pair in summaries[0]["statevector"]]
        expected = [1 / math.sqrt(2), 0, 0, 1 / math.sqrt(2)]
        check(len(amplitudes) == 4 and all(abs(a-b) < 1e-10 for a,b in zip(amplitudes, expected)),
              f"wrong coherent Bell state: {amplitudes}")
    else:
        config_path = elf.with_suffix(".run.json")
        config = json.loads(config_path.read_text())
        config["outcomes"] = [False, True]
        config_path.write_text(json.dumps(config))
        observed, _ = runner.run(elf, simulator, backend, 1, directory / "ordered")
        check(observed["counts"] == {"01": 1}, "result order changed")
    if backend == "mock":
        source = (root / "examples/bell.ll").read_text()
        lines = source.splitlines()
        positions = [i for i, line in enumerate(lines) if line.strip().startswith("call") and "result_record_output" in line]
        first, second = positions
        # Reverse records and repeat one output.
        lines[first], lines[second] = lines[second], lines[first]
        lines.insert(second + 1, lines[second])
        modified = "\n".join(lines).replace("array_record_output(i64 2", "array_record_output(i64 3")
        qir = directory / "reordered.ll"
        qir.write_text(modified)
        ordered_elf = directory / "reordered.elf"
        subprocess.run([compiler, qir, "--target", root / "targets/sim-default.json", "-o", ordered_elf], check=True)
        ordered_config = ordered_elf.with_suffix(".run.json")
        config = json.loads(ordered_config.read_text())
        config["outcomes"] = [False, True]
        ordered_config.write_text(json.dumps(config))
        observed, _ = runner.run(ordered_elf, simulator, backend, 1, directory / "reordered")
        check(observed["counts"] == {"100": 1}, "reordered or duplicated results changed")
        events = [json.loads(line) for line in (directory / "reordered/shot-0000.trace.jsonl").read_text().splitlines()]
        reads = [e for e in events if e["kind"] == "InstructionRetired" and e["word"] & 0x707F == 0x300B]
        check(len(reads) == 2, "duplicate output emitted an extra FMR")
        swapped = source.replace('mz__body(ptr null, ptr null)',
                                 'mz__body(ptr null, ptr inttoptr (i64 1 to ptr))')
        swapped = swapped.replace('mz__body(ptr inttoptr (i64 1 to ptr), ptr inttoptr (i64 1 to ptr))',
                                  'mz__body(ptr inttoptr (i64 1 to ptr), ptr null)')
        swapped_qir = directory / "swapped.ll"
        swapped_qir.write_text(swapped)
        target = json.loads((root / "targets/sim-default.json").read_text())
        for mapping in target["mappings"]:
            mapping["port"] += 3
        target_path = directory / "remapped.json"
        target_path.write_text(json.dumps(target))
        swapped_elf = directory / "swapped.elf"
        subprocess.run([compiler, swapped_qir, "--target", target_path, "-o", swapped_elf], check=True)
        config_path = swapped_elf.with_suffix(".run.json")
        config = json.loads(config_path.read_text())
        config["outcomes"] = [False, True]
        config_path.write_text(json.dumps(config))
        observed, _ = runner.run(swapped_elf, simulator, backend, 1, directory / "swapped")
        check(observed["counts"] == {"10": 1}, "FMR used a port or result ID as its qubit index")
        manifest_path = swapped_elf.with_suffix(".manifest.json")
        manifest = json.loads(manifest_path.read_text())
        manifest["abi"] = "unsupported"
        manifest_path.write_text(json.dumps(manifest))
        try:
            runner.run(swapped_elf, Path("/nonexistent-simulator"), backend, 1, directory / "bad-abi")
        except ValueError as error:
            check("ABI" in str(error), str(error))
        else:
            raise AssertionError("unsupported ABI accepted")
    # Compatibility checks must fail before launching any simulator process.
    config_path = elf.with_suffix(".run.json")
    config = json.loads(config_path.read_text())
    config["profile"]["start"] += 20
    config_path.write_text(json.dumps(config))
    try:
        runner.run(elf, Path("/nonexistent-simulator"), backend, 1, directory / "bad")
    except ValueError as error:
        check("profile" in str(error), str(error))
    else:
        raise AssertionError("target mismatch accepted")
    print(json.dumps(result))
