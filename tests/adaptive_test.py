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

    def compile_case(text, diagnostic=None):
        qir.write_text(text)
        result = subprocess.run(
            [compiler, qir, "--target", root / "targets/sim-default.json", "-o", elf],
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
