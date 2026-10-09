"""Execute LLVM-verified Bloq VM exports through the compiler and simulator."""

import copy
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pymatching
import stim

compiler, root, simulator, exporter = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("runner", root / "python/qsbit_compiler/runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def target_for(text, qubits):
    operations = set()
    for name, variant, args in re.findall(
        r"call void @__quantum__qis__(\w+)__(body|adj)\(([^\n]*)\)", text
    ):
        ids = [int(value) if value else 0 for value in re.findall(r"ptr (?:null|inttoptr \(i64 (\d+) to ptr\))", args)]
        operation = {"mz": "measure", "reset": "measure"}.get(name, name)
        if variant == "adj":
            operation += "dg"
        operations.add((operation, tuple(ids[:2] if name == "cx" else ids[:1])))
        if name == "reset":
            operations.add(("x", tuple(ids[:1])))
    mappings = []
    for codeword, (operation, ids) in enumerate(sorted(operations), 1):
        mappings.append({"operation": operation, "qubits": list(ids), "port": ids[0], "codeword": codeword, "duration_ns": 40 if operation == "measure" else 20})
    return {"schema": 1, "name": "bloq-adaptive", "qubits": qubits, "ports": qubits, "start_ns": 10000, "block_cycles": 1000, "mappings": mappings}


def memory_decoder(reference):
    matching = pymatching.Matching.from_detector_error_model(reference.detector_error_model(decompose_errors=True))
    edges = list(matching.to_networkx().edges(data=True))
    checks = np.zeros((reference.num_detectors, len(edges)), dtype=np.uint8)
    observables = np.zeros((reference.num_observables, len(edges)), dtype=np.uint8)
    weights = []
    for column, (a, b, data) in enumerate(edges):
        for node in (a, b):
            if node < reference.num_detectors:
                checks[node, column] ^= 1
        for observable in data["fault_ids"]:
            observables[observable, column] = 1
        weights.append(data["weight"])
    rows = np.zeros((reference.num_detectors, reference.num_measurements), dtype=np.uint8)
    count, detector = 0, 0
    for operation in reference.flattened():
        if operation.name == "DETECTOR":
            for operand in operation.targets_copy():
                rows[detector, count + operand.value] ^= 1
            detector += 1
        else:
            count += stim.Circuit(str(operation)).num_measurements
    return {"check_matrix": checks.tolist(), "observables": observables.tolist(), "measurement_to_detector": rows.tolist(), "weights": weights}


with tempfile.TemporaryDirectory(prefix="bloq-qir-integration-") as temporary:
    directory = Path(temporary)
    subprocess.run([exporter, directory], check=True)
    workloads = json.loads((directory / "workloads.json").read_text())
    reference = stim.Circuit((directory / "memory.reference.stim").read_text())
    matrix = memory_decoder(reference)
    decoder = json.loads((root / "examples/qec/repetition.target.json").read_text())["decoding"]
    verified = []
    for workload in workloads:
        name = workload["name"]
        text = (directory / f"{name}.ll").read_text()
        target = target_for(text, workload["qubits"])
        if name.startswith("repetition") or name in {"memory", "wait-exhausted"}:
            target["decoding"] = copy.deepcopy(decoder)
            if name == "memory":
                target["decoding"]["decoders"][0].update(measurements=reference.num_measurements, outputs=1, options=matrix)
        latencies = [1000, 1_000_000] if name.startswith("repetition") else ([1_000_000] if name == "wait-exhausted" else [1000])
        starts = []
        for latency in latencies:
            if "decoding" in target:
                target["decoding"]["decoders"][0]["latency"] = latency
            device = directory / f"{name}-{latency}.target.json"
            device.write_text(json.dumps(target))
            elf = directory / f"{name}-{latency}.elf"
            # Exercise both standard text and assembled bitcode inputs.
            source = directory / f"{name}.{'bc' if latency == 1000 else 'll'}"
            subprocess.run([compiler, source, "--target", device, "-o", elf], check=True)
            output = directory / f"run-{name}-{latency}"
            shots = 4 if name in {"retry", "phase", "memory", "products"} else 1
            result, summaries = runner.run(elf, simulator, workload["backend"], shots, output)
            if name.endswith("exhausted"):
                expected_code = "1" if name.startswith("retry") else "2"
                assert result["counts"] == {} and result["exit_codes"] == {expected_code: shots}, result
                assert all(summary["success"] and summary["exit_code"] == int(expected_code) for summary in summaries)
                events = [json.loads(line) for line in (output / "shot-0000.trace.jsonl").read_text().splitlines()]
                measured = [event for event in events if event["kind"] == "OperationStart" and event["operation"] == "measure"]
                if name == "retry-exhausted":
                    assert len(measured) == 6, measured  # Reset read plus authored read in each attempt.
                else:
                    assert len(measured) == 7, measured  # Three reads, two ancilla resets, two checks.
                continue
            assert all(summary["exit_code"] == 0 for summary in summaries), summaries
            if name == "feedback":
                assert result["counts"] == {"11": 1}, result
                trace = json.loads((directory / "feedback.vm-trace.json").read_text())
                assert [int(item["value"]) for item in trace["measurements"]] == [1, 1], trace
            elif name == "phase":
                assert result["counts"] == {"00": shots}, result
            elif name == "products":
                assert set(result["counts"]).issubset({"000000", "000011"}), result
                trace = json.loads((directory / "products.vm-trace.json").read_text())
                values = [int(item["value"]) for item in trace["measurements"]]
                assert values[:4] == [0, 0, 0, 0] and values[4] == values[5], values
            elif name == "early-retry":
                assert result["counts"] == {"11": shots}, result
                trace = json.loads((directory / "early-retry.vm-trace.json").read_text())
                assert [int(item["value"]) for item in trace["measurements"]] == [0, 1], trace
            elif name == "retry":
                assert result["counts"] == {"1": shots}, result
            elif name.startswith("repetition"):
                error = int(name.rsplit("-", 1)[1])
                syndrome = {0: "10", 1: "11", 2: "01"}[error]
                assert result["counts"] == {syndrome + "1" + "000" + "01": 1}, result
                events = [json.loads(line) for line in (output / "shot-0000.trace.jsonl").read_text().splitlines()]
                jobs = [event for event in events if event["kind"] == "DecoderStarted"]
                assert len(jobs) == 1, jobs  # Corrected and Flip share one solve.
                completed = [event for event in events if event["kind"] == "DecoderCompleted"]
                assert completed[0]["tick"] - jobs[0]["tick"] == latency
                acquisitions = [event for event in events if event["kind"] == "OperationStart" and event["operation"] == "measure"]
                starts.append(len(acquisitions))
                if latency > 1000:
                    assert any(jobs[0]["tick"] < event["tick"] < completed[0]["tick"] for event in acquisitions), acquisitions
                    assert len(acquisitions) > 6, acquisitions
            elif name == "memory":
                converter = reference.compile_m2d_converter()
                matching = pymatching.Matching(np.array(matrix["check_matrix"], dtype=np.uint8), weights=matrix["weights"], faults_matrix=np.array(matrix["observables"], dtype=np.uint8))
                for bitstring, count in result["counts"].items():
                    values = np.array([[int(bit) for bit in bitstring[:reference.num_measurements]]], dtype=np.bool_)
                    detectors, logical = converter.convert(measurements=values, separate_observables=True)
                    predicted = int(matching.decode(detectors[0])[0])
                    assert not detectors.any(), detectors
                    assert int(bitstring[-2]) == int(logical[0, 0])
                    assert int(bitstring[-1]) == (int(logical[0, 0]) ^ predicted) == 0
            verified.append({"workload": name, "latency": latency, "counts": result["counts"]})
        if name.startswith("repetition"):
            assert starts[1] > starts[0], starts
    # Exhaustion is a program exit status, not an ISA fault or an accepted shot.
    text = (directory / "feedback.ll").read_text()
    text = text.replace("ret i64 0", "ret i64 1")
    text = re.sub(r"^\s*call void @__quantum__rt__bool_record_output[^\n]*\n", "", text, flags=re.MULTILINE)
    rejected = directory / "rejected.ll"
    rejected.write_text(text)
    elf = directory / "rejected.elf"
    device = directory / "rejected.target.json"
    device.write_text(json.dumps(target_for(text, 2)))
    subprocess.run([compiler, rejected, "--target", device, "-o", elf], check=True)
    for cpu in ("rv32", "vliw"):
        config_path = elf.with_suffix(".run.json")
        config = json.loads(config_path.read_text())
        config["cpu_model"] = cpu
        config_path.write_text(json.dumps(config))
        result, summaries = runner.run(elf, simulator, "stim", 1, directory / f"rejected-{cpu}")
        assert result["counts"] == {} and result["exit_codes"] == {"1": 1}, result
        assert summaries[0]["success"] and summaries[0]["exit_code"] == 1
        assert summaries[0]["cpu_model"] == cpu
    print(json.dumps(verified, indent=2))
