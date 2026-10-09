#!/usr/bin/env python3
"""Run a compiler artifact bundle after checking its executable and target profile."""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
from collections import Counter
from pathlib import Path


def run(elf, simulator, backend, shots, output, seed=1):
    elf = Path(elf).resolve()
    output = Path(output).resolve()
    manifest = json.loads(elf.with_suffix(".manifest.json").read_text())
    config = json.loads(elf.with_suffix(".run.json").read_text())
    if manifest.get("abi") not in ("qsbit-static-v2", "qsbit-adaptive-v1"):
        raise ValueError("unsupported executable ABI")
    adaptive = manifest["abi"] == "qsbit-adaptive-v1"
    if hashlib.sha256(elf.read_bytes()).hexdigest() != manifest["elf_sha256"]:
        raise ValueError("ELF does not match its manifest")
    if config["profile"] != manifest["profile"]:
        raise ValueError("run profile does not match the compiled target")
    if config.get("decoding") != manifest.get("decoding"):
        raise ValueError("run decoder does not match the compiled target")
    if (elf.parent / config["program"]).resolve() != elf:
        raise ValueError("run configuration refers to a different ELF")
    addresses = [
        record["address"]
        for record in manifest["outputs"]
        if record["kind"] == "result"
    ]
    layout = manifest.get("output_buffer")
    if config["inspect"] != ([layout["count_address"]] if adaptive else addresses):
        raise ValueError("run result layout does not match the manifest")
    if shots < 1 or seed < 0 or seed + shots - 1 > 0xFFFFFFFF:
        raise ValueError("invalid shot count or seed range")
    selected = shutil.which(str(simulator))
    if selected is None:
        raise ValueError(
            f"simulator not found: {simulator}; install qsbit-sim or pass --sim PATH"
        )
    simulator = str(Path(selected).resolve())
    output.mkdir(parents=True, exist_ok=True)
    counts = Counter()
    summaries = []
    exit_codes = Counter()
    for shot in range(shots):
        current = dict(config)
        current["profile"] = dict(config["profile"], seed=seed + shot)
        current["program"] = str(elf)
        current["backend"] = backend
        summary_path = output / f"shot-{shot:04d}.summary.json"
        current["summary"] = str(summary_path)
        current["trace"] = str(output / f"shot-{shot:04d}.trace.jsonl")
        if adaptive:
            current["memory_dump"] = str(output / f"shot-{shot:04d}.memory.bin")
        run_path = output / f"shot-{shot:04d}.run.json"
        run_path.write_text(json.dumps(current, indent=2) + "\n")
        subprocess.run(
            [str(simulator), "--config", str(run_path)],
            check=True,
            capture_output=True,
            text=True,
        )
        summary = json.loads(summary_path.read_text())
        if not summary["success"] or any(
            reg["pending"] for reg in summary["measurement_registers"]
        ):
            raise ValueError(f"shot {shot} has unfinished measurements")
        exit_code = summary.get("exit_code", 0)
        if exit_code:
            exit_codes[str(exit_code)] += 1
            summaries.append(summary)
            continue
        values = [summary["memory"][str(address)] for address in addresses]
        if adaptive:
            memory = Path(current["memory_dump"]).read_bytes()
            length = struct.unpack_from("<I", memory, layout["count_address"])[0]
            if length > layout["capacity"]:
                raise ValueError("output count exceeds buffer capacity")
            values = struct.unpack_from(f"<{length}I", memory, layout["data_address"])
        if any(value not in (0, 1) for value in values):
            raise ValueError(f"shot {shot} produced a non-bit output")
        counts["".join(str(value) for value in values)] += 1
        summaries.append(summary)
    result = {
        "shots": shots,
        "backend": backend,
        "seed": seed,
        "bit_order": "QIR result_record_output order",
        "counts": dict(sorted(counts.items())),
    }
    if exit_codes:
        result["exit_codes"] = dict(sorted(exit_codes.items()))
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result, summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument(
        "--sim",
        default="qsbit-sim",
        help="simulator command or path (default: PATH lookup)",
    )
    parser.add_argument("--backend", default="aer")
    parser.add_argument("--shots", type=int, default=16)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out-dir", type=Path, default=Path("out/runs"))
    args = parser.parse_args()
    try:
        result, _ = run(
            args.elf, args.sim, args.backend, args.shots, args.out_dir, args.seed
        )
    except subprocess.CalledProcessError as error:
        parser.exit(
            1,
            f"qsbit-run: simulator exited with status {error.returncode}\n{error.stderr}",
        )
    except (ValueError, OSError) as error:
        parser.exit(1, f"qsbit-run: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
