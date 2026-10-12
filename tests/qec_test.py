"""Execute adaptive loops and decoder feedback on the simulator."""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root, simulator = map(Path, sys.argv[1:])
sys.path.insert(0, str(root / "python"))
from qsbit_compiler import runner

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
    late["decoding"]["decoders"][0]["latency"] = 100000
    elf = compile_case("late", source, late)
    result, _ = runner.run(elf, simulator, "stim", 1, directory / "late")
    assert result["counts"] == {"11000" + "00000" * 2: 1}, result
    events = [
        json.loads(line)
        for line in (directory / "late/shot-0000.trace.jsonl").read_text().splitlines()
    ]
    assert any(e["kind"] == "TimerPaused" for e in events)
    returned = [e["tick"] for e in events if e["kind"] == "DecoderResultReturned"]
    assert len(returned) == 3 and returned[-1] > 300000, returned

    # Native reset groups and measurement batches preserve output ordering at capacity.
    batch_target = json.loads((root / "targets/sim-default.json").read_text())
    batch_target.update(qubits=9, ports=9, cpu_period_ns=2, tcu_period_ns=4, block_cycles=100)
    batch_target["mappings"] = [
        dict(operation=name, qubits=[q], port=q, codeword=code, duration_ns=duration)
        for q in range(9)
        for name, code, duration in (("measure", 4, 40), ("reset", 5, 80), ("x", 1, 20))
    ]
    def pointer(q):
        return "ptr null" if q == 0 else f"ptr inttoptr (i64 {q} to ptr)"

    calls = [f"call void @__quantum__qis__reset__body({pointer(q)})" for q in range(9)]
    calls += [f"call void @__quantum__qis__x__body({pointer(q)})" for q in range(0, 9, 2)]
    calls += [f"call void @__quantum__qis__mz__body({pointer(q)}, {pointer(q)})" for q in range(9)]
    calls += [f"call void @__quantum__rt__result_record_output({pointer(q)}, ptr null)" for q in range(9)]
    batch_source = "\n".join([
        "declare void @__quantum__qis__reset__body(ptr)",
        "declare void @__quantum__qis__x__body(ptr)",
        "declare void @__quantum__qis__mz__body(ptr, ptr)",
        "declare void @__quantum__rt__result_record_output(ptr, ptr)",
        "define void @batch() #0 {", "entry:", *calls, "ret void", "}",
        'attributes #0 = { "entry_point" "qir_profiles"="adaptive_profile" "required_num_qubits"="9" "required_num_results"="9" }',
    ])
    batch_elf = compile_case("batch", batch_source, batch_target)
    observed, _ = runner.run(batch_elf, simulator, "stim", 1, directory / "batch")
    assert observed["counts"] == {"101010101": 1}, observed
    events = [json.loads(line) for line in (directory / "batch/shot-0000.trace.jsonl").read_text().splitlines()]
    measured = [e for e in events if e["kind"] == "OperationStart" and e["operation"] == "measure"]
    resets = [e for e in events if e["kind"] == "OperationStart" and e["operation"] == "reset"]
    assert len(resets) == len(measured) == 9
    assert len({e["tick"] for e in resets}) == 1
    assert len({e["tick"] for e in measured[:8]}) == 1
    assert measured[8]["tick"] > measured[0]["tick"]
    assert len([e for e in events if e["kind"] == "MeasurementSampled"]) == 9
    # SystemC callback registration order cannot alter grouped results or event timing.
    run_path = batch_elf.with_suffix(".run.json")
    run_config = json.loads(run_path.read_text())
    run_config["reverse_registration"] = True
    run_path.write_text(json.dumps(run_config))
    reversed_result, _ = runner.run(batch_elf, simulator, "stim", 1, directory / "batch-reverse")
    assert reversed_result == observed
    reversed_events = [json.loads(line) for line in (directory / "batch-reverse/shot-0000.trace.jsonl").read_text().splitlines()]
    def measurement_timing(trace):
        return [(e["tick"], e["operation"], e["targets"]) for e in trace
                if e["kind"] == "OperationStart"]
    assert measurement_timing(reversed_events) == measurement_timing(events)
    subprocess.run([compiler, directory / "batch.ll", "--target", directory / "batch.target.json",
                    "-o", batch_elf, "--no-batch-operations"], check=True)
    scalar_result, _ = runner.run(batch_elf, simulator, "stim", 1, directory / "batch-scalar")
    assert scalar_result["counts"] == observed["counts"]
    scalar_events = [json.loads(line) for line in (directory / "batch-scalar/shot-0000.trace.jsonl").read_text().splitlines()]
    assert sum(e["kind"] == "CodewordQueued" for e in events) < sum(
        e["kind"] == "CodewordQueued" for e in scalar_events)
    batch_summary = json.loads((directory / "batch/shot-0000.summary.json").read_text())
    scalar_summary = json.loads((directory / "batch-scalar/shot-0000.summary.json").read_text())
    assert batch_summary["stop_tick"] < scalar_summary["stop_tick"]
