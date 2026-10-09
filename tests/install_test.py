"""Compile outside the checkout using a relocated CMake installation."""

import subprocess
import sys
import tempfile
from pathlib import Path

build, source, bindir, datadir = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="qsbitc-install-") as temporary:
    directory = Path(temporary)
    prefix = directory / "prefix"
    subprocess.run(["cmake", "--install", build, "--prefix", str(prefix)], check=True)
    relocated = directory / "relocated"
    prefix.rename(relocated)
    subprocess.run(
        [
            str(relocated / bindir / "qsbitc"),
            str(Path(source) / "examples/bell.ll"),
            "--target",
            str(relocated / datadir / "qsbit-compiler/targets/sim-default.json"),
            "-o",
            str(directory / "bell.elf"),
        ],
        cwd=directory,
        check=True,
    )
    assert (directory / "bell.manifest.json").is_file()
