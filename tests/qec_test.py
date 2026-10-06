"""Execute adaptive loops and decoder feedback on the simulator."""

import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root, simulator = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("runner", root / "tools/run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

with tempfile.TemporaryDirectory(prefix="qsbit-qec-") as temporary:
    directory = Path(temporary)

    def compile_case(name, source, target):
        qir, device, elf = (
            directory / f"{name}{ext}" for ext in (".ll", ".target.json", ".elf")
        )
        qir.write_text(source)
        device.write_text(json.dumps(target))
        subprocess.run([compiler, qir, "--target", device, "-o", elf], check=True)
        return elf

    feedback = compile_case(
        "feedback",
        (root / "examples/qec/feedback.ll").read_text(),
        json.loads((root / "targets/sim-default.json").read_text()),
    )
    result, _ = runner.run(feedback, simulator, "stim", 1, directory / "feedback")
    assert result["counts"] == {"100": 1}, result
    config_path = feedback.with_suffix(".run.json")
    config = json.loads(config_path.read_text())
    config["resets"] = [60000]
    config_path.write_text(json.dumps(config))
    result, _ = runner.run(feedback, simulator, "stim", 1, directory / "feedback-reset")
    assert result["counts"] == {"100": 1}, result

    parallel = [
        "declare void @__quantum__qis__h__body(ptr)",
        "define void @parallel() #0 {",
        "entry:",
    ]
    mappings = []
    for qubit in range(20):
        pointer = "ptr null" if qubit == 0 else f"ptr inttoptr (i64 {qubit} to ptr)"
        parallel.append(f"call void @__quantum__qis__h__body({pointer})")
        mappings.append(
            {
                "operation": "h",
                "qubits": [qubit],
                "port": qubit,
                "codeword": 1,
                "duration_ns": 20,
            }
        )
    parallel.extend(
        [
            "ret void",
            "}",
            'attributes #0 = { "entry_point" "qir_profiles"="adaptive_profile" "required_num_qubits"="20" "required_num_results"="0" }',
        ]
    )
    elf = compile_case(
        "parallel",
        "\n".join(parallel),
        {
            "schema": 1,
            "name": "parallel",
            "qubits": 20,
            "ports": 20,
            "start_ns": 10000,
            "mappings": mappings,
        },
    )
    runner.run(elf, simulator, "mock", 1, directory / "parallel")
    events = [
        json.loads(line)
        for line in (directory / "parallel/shot-0000.trace.jsonl")
        .read_text()
        .splitlines()
    ]
    assert [event["tick"] for event in events if event["kind"] == "OperationStart"] == [
        10000
    ] * 16 + [10020] * 4
    source = (root / "examples/qec/repetition.ll").read_text()
    target = json.loads((root / "examples/qec/repetition.target.json").read_text())
    for error, syndrome in ((0, "10"), (1, "11"), (2, "01")):
        pointer = "ptr null" if error == 0 else f"ptr inttoptr (i64 {error} to ptr)"
        text = source.replace(
            "x__body(ptr inttoptr (i64 1 to ptr))", f"x__body({pointer})", 1
        )
        elf = compile_case(f"error-{error}", text, target)
        output = directory / f"run-{error}"
        result, _ = runner.run(elf, simulator, "stim", 1, output)
        assert result["counts"] == {syndrome + "000" + "00000" * 2: 1}, result
        events = [
            json.loads(line)
            for line in (output / "shot-0000.trace.jsonl").read_text().splitlines()
        ]
        starts = [e for e in events if e["kind"] == "DecoderStarted"]
        completed = [e for e in events if e["kind"] == "DecoderCompleted"]
        assert len(starts) == len(completed) == 3
        assert all(b["tick"] - a["tick"] == 1000 for a, b in zip(starts, completed))
        if error == 1:
            config_path = elf.with_suffix(".run.json")
            config = json.loads(config_path.read_text())
            config["reverse_registration"] = True
            config_path.write_text(json.dumps(config))
            reversed_result, _ = runner.run(
                elf, simulator, "stim", 1, directory / "reversed"
            )
            reversed_events = [
                json.loads(line)
                for line in (directory / "reversed/shot-0000.trace.jsonl")
                .read_text()
                .splitlines()
            ]
            assert reversed_result == result
            assert [e for e in events if e["kind"].startswith("Decoder")] == [
                e for e in reversed_events if e["kind"].startswith("Decoder")
            ]

    late = copy.deepcopy(target)
    late["decoding"]["decoders"][0]["latency"] = 1000000
    elf = compile_case("late", source, late)
    process = subprocess.run(
        [simulator, "--config", elf.with_suffix(".run.json")],
        capture_output=True,
        check=False,
    )
    summary = json.loads(elf.with_suffix(".summary.json").read_text())
    assert process.returncode != 0 and summary["fault"] == "LateAdmission", summary
