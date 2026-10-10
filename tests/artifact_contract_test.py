"""Check packaged contracts and reject malformed executable bundles before execution."""

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, source, contract_data = map(Path, sys.argv[1:])
package = source / "python/qsbit_compiler"
for name in ("executable.json", "artifact.schema.json"):
    assert json.loads((package / name).read_text()) == json.loads(
        (contract_data / name).read_text()
    ), name
spec = importlib.util.spec_from_file_location("runner", package / "runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
with tempfile.TemporaryDirectory() as temporary:
    elf = Path(temporary) / "feedback.elf"
    subprocess.run(
        [
            str(compiler),
            str(source / "examples/qec/feedback.ll"),
            "--target",
            str(source / "targets/sim-default.json"),
            "-o",
            str(elf),
        ],
        check=True,
    )
    runner.load_bundle(elf)
    path = elf.with_suffix(".manifest.json")
    original = json.loads(path.read_text())
    for mutation in ("layout", "schema", "type"):
        manifest = json.loads(json.dumps(original))
        if mutation == "layout":
            manifest["output_buffer"]["data_address"] += 4
        elif mutation == "schema":
            manifest["schema"] = 2
        else:
            manifest["profile"]["ports"] = "1"
        path.write_text(json.dumps(manifest))
        try:
            runner.load_bundle(elf)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid {mutation}")
