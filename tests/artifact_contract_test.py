"""Check packaged contracts and reject malformed executable bundles before execution."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

compiler, source, contract_data = map(Path, sys.argv[1:])
package = source / "python/qsbit_compiler"
for name in ("executable.json", "artifact.schema.json"):
    assert json.loads((package / name).read_text()) == json.loads(
        (contract_data / name).read_text()
    ), name
sys.path.insert(0, str(package.parent))
from qsbit_compiler.artifact import load_bundle, main


def validate_cli(elf, status):
    with patch.object(sys, "argv", ["qsbit-validate", str(elf)]):
        try:
            main()
        except SystemExit as error:
            assert error.code == status
        else:
            assert status == 0


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
    load_bundle(elf)
    validate_cli(elf, 0)
    path = elf.with_suffix(".manifest.json")
    original = json.loads(path.read_text())
    for mutation in ("layout", "schema", "type", "digest", "profile"):
        manifest = json.loads(json.dumps(original))
        if mutation == "layout":
            manifest["output_buffer"]["data_address"] += 4
        elif mutation == "schema":
            manifest["schema"] = 2
        elif mutation == "type":
            manifest["profile"]["ports"] = "1"
        elif mutation == "digest":
            manifest["elf_sha256"] = "0" * 64
        else:
            manifest["profile"]["seed"] += 1
        path.write_text(json.dumps(manifest))
        try:
            load_bundle(elf)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid {mutation}")
    validate_cli(elf, 1)
