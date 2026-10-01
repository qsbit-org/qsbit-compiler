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
spec = importlib.util.spec_from_file_location("qsbit_runner", root / "tools/run.py")
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
    check(len(reads) == 2, "each measurement handle must be consumed exactly once")
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
        # Reverse records and repeat one result: QREAD must still consume only two handles.
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
        check(len(reads) == 2, "duplicate output consumed a measurement handle twice")
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
