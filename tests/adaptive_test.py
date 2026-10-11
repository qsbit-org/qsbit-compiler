"""Adaptive QIR validation and artifact contracts."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root = map(Path, sys.argv[1:])
source = (root / "examples/qec/feedback.ll").read_text()

with tempfile.TemporaryDirectory(prefix="qsbit-adaptive-") as temporary:
    directory = Path(temporary)
    qir, elf = directory / "input.ll", directory / "output.elf"

    def compile_case(text, diagnostic=None, target=None):
        qir.write_text(text)
        result = subprocess.run(
            [compiler, qir, "--target", target or root / "targets/sim-default.json", "-o", elf],
            text=True,
            capture_output=True,
            check=False,
        )
        if diagnostic is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0 and diagnostic in result.stderr, result.stderr

    compile_case(source)
    manifest = json.loads(elf.with_suffix(".manifest.json").read_text())
    run = json.loads(elf.with_suffix(".run.json").read_text())
    assert manifest["abi"] == "qsbit-adaptive-v1"
    assert manifest["profile"] == run["profile"]
    assert manifest["output_buffer"]["count_address"] == run["inspect"][0]
    assert (
        json.loads(elf.with_suffix(".schedule.json").read_text())["mode"]
        == "control-flow"
    )
    compile_case(
        source.replace('"required_num_qubits"="1"', '"required_num_qubits"="0"'),
        "exceeds declared",
    )
    compile_case(
        source.replace('!"qir_major_version", i32 1', '!"qir_major_version", i32 2'),
        "QIR 1.0",
    )
    compile_case(source.replace("i1 false", "i1 true"), "dynamic allocation")
    compile_case(
        source.replace(
            "  call void @__quantum__qis__mz__body(ptr null, ptr null)\n", ""
        ),
        "unmeasured result",
    )
    compile_case(
        source.replace(
            "entry:\n",
            "entry:\n  %early = call i1 @__quantum__rt__read_result(ptr null)\n",
        ),
        "unmeasured result",
    )
    compile_case(
        source.replace("%next = add i32", "%next = mul i32"),
        "unsupported classical instruction",
    )
    compile_case(
        source.replace(
            "call void @__quantum__qis__x__body(ptr null)", "call void @feedback()"
        ),
        "recursive",
    )
    compile_case(source + "\n@mutable = global i32 0\n", "mutable input globals")
    helper = source.replace(
        "call void @__quantum__qis__x__body(ptr null)", "call void @flip()"
    )
    helper += "\ndefine void @flip() { call void @__quantum__qis__x__body(ptr null)\nret void }\n"
    compile_case(helper)
    # Only one incoming branch measures result 0.
    branch = source.replace(
        "  call void @__quantum__qis__x__body(ptr null)\n  br label %round",
        "  br i1 true, label %measured, label %round\nmeasured:\n"
        "  call void @__quantum__qis__mz__body(ptr null, ptr null)\n  br label %round",
        1,
    )
    branch = branch.replace("[0, %entry]", "[0, %entry], [0, %measured]")
    branch = branch.replace(
        "  call void @__quantum__qis__mz__body(ptr null, ptr null)\n  %bit", "  %bit"
    )
    compile_case(branch, "unmeasured result")

    # Standard QIR 2.1 uses an i64 exit code and non-null constant labels.
    standard = source.replace("define void @feedback()", "define i64 @feedback()")
    standard = standard.replace("ret void", "ret i64 0")
    standard = standard.replace('!"qir_major_version", i32 1', '!"qir_major_version", i32 2')
    standard = standard.replace('!"qir_minor_version", i32 0', '!"qir_minor_version", i32 1')
    # QIR 2.1 output is terminal, after the measurement-feedback loop.
    standard = standard.replace("  call void @__quantum__rt__result_record_output(ptr null, ptr null)\n", "")
    standard = standard.replace("  ret i64 0", "  call void @__quantum__rt__result_record_output(ptr null, ptr @label)\n  ret i64 0")
    standard += '\n@label = private constant [4 x i8] c"out\\00"\n'
    compile_case(standard)
    compile_case(standard.replace("define i64 @feedback()", "define void @feedback()").replace("ret i64 0", "ret void"), "must return i64")
    compile_case(standard.replace('!"qir_minor_version", i32 1', '!"qir_minor_version", i32 99'), "supported Adaptive versions")
    compile_case(source.replace("call void @__quantum__qis__x__body(ptr null)", "call i1 @decoder_ready_ui64(i64 0)") + "\ndeclare i1 @decoder_ready_ui64(i64)\n", "target.decoding")

    compile_case(standard.replace("ret i64 0", "ret i64 64"), "exit status")
    compile_case(standard.replace("ret i64 0", "ret i64 4294967296"), "exit status")
    ready = standard.replace("call void @__quantum__qis__x__body(ptr null)", "call i1 @decoder_ready_ui64(i64 0)") + "\ndeclare i1 @decoder_ready_ui64(i64)\n"
    decoder_target = root / "examples/qec/repetition.target.json"
    compile_case(ready, target=decoder_target)
    compile_case(ready.replace("call i1 @decoder_ready_ui64", "call i64 @decoder_ready_ui64").replace("declare i1 @decoder_ready_ui64", "declare i64 @decoder_ready_ui64"), "invalid decoder function signature", decoder_target)
    compile_case(ready.replace("decoder_ready_ui64(i64 0)", "decoder_ready_ui64(i32 0)").replace("decoder_ready_ui64(i64)", "decoder_ready_ui64(i32)"), "decoder arguments must be i64", decoder_target)

    # Nine distinct measurements require an eight-result batch and one scalar command.
    batch_target = json.loads((root / "targets/sim-default.json").read_text())
    batch_target.update(qubits=9, ports=9)
    batch_target["mappings"] = [
        dict(operation=name, qubits=[q], port=q, codeword=code, duration_ns=duration)
        for q in range(9)
        for name, code, duration in (("measure", 4, 40), ("reset", 5, 80))
    ]
    batch_device = directory / "batch.target.json"
    batch_device.write_text(json.dumps(batch_target))
    def ptr(q):
        return "ptr null" if q == 0 else f"ptr inttoptr (i64 {q} to ptr)"
    declarations = "declare void @__quantum__qis__mz__body(ptr, ptr)\ndeclare void @__quantum__qis__reset__body(ptr)\n"
    reset_calls = [f"call void @__quantum__qis__reset__body({ptr(q)})" for q in range(9)]
    measure_calls = [f"call void @__quantum__qis__mz__body({ptr(q)}, {ptr(q)})" for q in range(9)]
    batch_source = declarations + "define void @batch() #0 {\nentry:\n" + "\n".join(
        reset_calls + measure_calls) + '\nret void\n}\nattributes #0 = { "entry_point" "qir_profiles"="adaptive_profile" "required_num_qubits"="9" "required_num_results"="9" }'
    compile_case(batch_source, target=batch_device)
    profile = json.loads(elf.with_suffix(".run.json").read_text())["profile"]
    groups = [m for m in profile["mappings"] if len(m["actions"]) > 1]
    assert sorted(len(m["actions"]) for m in groups) == [8, 9]
    scheduled_groups = json.loads(elf.with_suffix(".schedule.json").read_text())["groups"]
    assert [(g["operation"], len(g["targets"])) for g in scheduled_groups] == [("reset", 9), ("measure", 8)]
    assert [[a["targets"][0] for a in m["actions"]] for m in groups] == [
        list(range(9)), list(range(8))], groups
    run = subprocess.run([compiler, qir, "--target", batch_device, "-o", elf,
                          "--no-batch-operations"], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    profile = json.loads(elf.with_suffix(".run.json").read_text())["profile"]
    assert all(len(m["actions"]) == 1 for m in profile["mappings"])
    # Result reads prevent moving later measurements before an observable use.
    guarded = batch_source.replace(declarations, declarations + "declare i1 @__quantum__rt__read_result(ptr)\n")
    guarded = guarded.replace(measure_calls[0], measure_calls[0] + "\n%bit = call i1 @__quantum__rt__read_result(ptr null)")
    compile_case(guarded, target=batch_device)
    profile = json.loads(elf.with_suffix(".run.json").read_text())["profile"]
    measure_groups = [m for m in profile["mappings"] if len(m["actions"]) > 1
                      and m["actions"][0]["kind"] == "acquire"]
    assert len(measure_groups) == 1
    assert [a["targets"][0] for a in measure_groups[0]["actions"]] == list(range(1, 9))
    missing_reset = dict(batch_target, mappings=[m for m in batch_target["mappings"]
                                               if m["operation"] != "reset"])
    batch_device.write_text(json.dumps(missing_reset))
    compile_case(batch_source, diagnostic="mapping", target=batch_device)
