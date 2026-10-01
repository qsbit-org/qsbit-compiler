#!/usr/bin/env python3
"""Run a compiler artifact bundle after checking its executable and target profile."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess


def run(elf, simulator, backend, shots, output, seed=1):
    elf = Path(elf).resolve()
    output = Path(output).resolve()
    manifest = json.loads(elf.with_suffix(".manifest.json").read_text())
    config = json.loads(elf.with_suffix(".run.json").read_text())
    if manifest.get("abi") != "qsbit-static-v1":
        raise ValueError("unsupported executable ABI")
    if hashlib.sha256(elf.read_bytes()).hexdigest() != manifest["elf_sha256"]:
        raise ValueError("ELF does not match its manifest")
    if config["profile"] != manifest["profile"]:
        raise ValueError("run profile does not match the compiled target")
    if (elf.parent / config["program"]).resolve() != elf:
        raise ValueError("run configuration refers to a different ELF")
    addresses = [record["address"] for record in manifest["outputs"] if record["kind"] == "result"]
    if config["inspect"] != addresses:
        raise ValueError("run result layout does not match the manifest")
    if shots < 1 or seed < 0 or seed + shots - 1 > 0xFFFFFFFF:
        raise ValueError("invalid shot count or seed range")
    output.mkdir(parents=True, exist_ok=True)
    counts = Counter()
    summaries = []
    for shot in range(shots):
        current = dict(config)
        current["profile"] = dict(config["profile"], seed=seed + shot)
        current["program"] = str(elf)
        current["backend"] = backend
        summary_path = output / f"shot-{shot:04d}.summary.json"
        current["summary"] = str(summary_path)
        current["trace"] = str(output / f"shot-{shot:04d}.trace.jsonl")
        run_path = output / f"shot-{shot:04d}.run.json"
        run_path.write_text(json.dumps(current, indent=2) + "\n")
        subprocess.run([str(simulator), "--config", str(run_path)], check=True, capture_output=True)
        summary = json.loads(summary_path.read_text())
        if not summary["success"] or summary["result_slots"]:
            raise ValueError(f"shot {shot} did not complete and release all result handles")
        values = [summary["memory"][str(address)] for address in addresses]
        if any(value not in (0, 1) for value in values):
            raise ValueError(f"shot {shot} produced a non-bit output")
        counts["".join(str(value) for value in values)] += 1
        summaries.append(summary)
    result = {"shots": shots, "backend": backend, "seed": seed,
              "bit_order": "QIR result_record_output order", "counts": dict(sorted(counts.items()))}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    return result, summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--sim", required=True, type=Path)
    parser.add_argument("--backend", choices=("aer", "mock"), default="aer")
    parser.add_argument("--shots", type=int, default=16)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--out-dir", type=Path, default=Path("out/runs"))
    args = parser.parse_args()
    try:
        result, _ = run(args.elf, args.sim, args.backend, args.shots, args.out_dir, args.seed)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"qsbit runner: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
