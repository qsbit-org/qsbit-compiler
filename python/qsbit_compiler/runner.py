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
from typing import NamedTuple

from jsonschema import Draft202012Validator

CONTRACT = json.loads(Path(__file__).with_name("executable.json").read_text())
SCHEMA = json.loads(Path(__file__).with_name("artifact.schema.json").read_text())


def validate_artifact(value, kind):
    validator = Draft202012Validator(dict(SCHEMA, **{"$ref": f"#/$defs/{kind}"}))
    errors = sorted(validator.iter_errors(value), key=lambda error: str(error.path))
    if errors:
        raise ValueError(f"invalid {kind}: {errors[0].message}")


class ArtifactBundle(NamedTuple):
    elf: Path
    config: dict
    adaptive: bool
    addresses: list
    layout: dict | None


class Shot(NamedTuple):
    config: dict
    run_path: Path
    summary_path: Path


def load_bundle(elf):
    elf = Path(elf).resolve()
    manifest = json.loads(elf.with_suffix(".manifest.json").read_text())
    config = json.loads(elf.with_suffix(".run.json").read_text())
    validate_artifact(manifest, "manifest")
    validate_artifact(config, "run")
    if manifest.get("abi") not in (
        CONTRACT["static"]["identifier"],
        CONTRACT["adaptive"]["identifier"],
    ):
        raise ValueError("unsupported executable ABI")
    adaptive = manifest["abi"] == CONTRACT["adaptive"]["identifier"]
    expected = CONTRACT["adaptive" if adaptive else "static"]
    if (
        manifest["isa"] != CONTRACT["isa"]
        or manifest["stack_pointer"] != expected["stack_pointer"]
    ):
        raise ValueError("executable layout does not match the ABI")
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
    if adaptive and layout != {
        "count_address": expected["output_count"],
        "data_address": expected["output_data"],
        "capacity": expected["output_capacity"],
        "word_bytes": CONTRACT["word_bytes"],
    }:
        raise ValueError("output layout does not match the ABI")
    if adaptive and config.get("memory_size") != expected["memory_size"]:
        raise ValueError("memory size does not match the ABI")
    if config["inspect"] != ([layout["count_address"]] if adaptive else addresses):
        raise ValueError("run result layout does not match the manifest")
    return ArtifactBundle(elf, config, adaptive, addresses, layout)


def prepare_shot(bundle, backend, output, shot, seed):
    current = dict(bundle.config)
    current["profile"] = dict(bundle.config["profile"], seed=seed)
    current["program"] = str(bundle.elf)
    current["backend"] = backend
    summary_path = output / f"shot-{shot:04d}.summary.json"
    current["summary"] = str(summary_path)
    current["trace"] = str(output / f"shot-{shot:04d}.trace.jsonl")
    if bundle.adaptive:
        current["memory_dump"] = str(output / f"shot-{shot:04d}.memory.bin")
    return Shot(current, output / f"shot-{shot:04d}.run.json", summary_path)


def execute_shot(simulator, shot):
    shot.run_path.write_text(json.dumps(shot.config, indent=2) + "\n")
    subprocess.run(
        [str(simulator), "--config", str(shot.run_path)],
        check=True,
        capture_output=True,
        text=True,
    )
    summary = json.loads(shot.summary_path.read_text())
    if not summary["success"] or any(
        register["pending"] for register in summary["measurement_registers"]
    ):
        raise ValueError(f"{shot.summary_path.name} has unfinished measurements")
    return summary


def decode_result(bundle, shot, summary):
    if bundle.adaptive:
        memory = Path(shot.config["memory_dump"]).read_bytes()
        length = struct.unpack_from("<I", memory, bundle.layout["count_address"])[0]
        if length > bundle.layout["capacity"]:
            raise ValueError("output count exceeds buffer capacity")
        values = struct.unpack_from(
            f"<{length}I", memory, bundle.layout["data_address"]
        )
    else:
        values = [summary["memory"][str(address)] for address in bundle.addresses]
    if any(value not in (0, 1) for value in values):
        raise ValueError(f"{shot.summary_path.name} produced a non-bit output")
    return "".join(str(value) for value in values)


def run(elf, simulator, backend, shots, output, seed=1):
    bundle = load_bundle(elf)
    output = Path(output).resolve()
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
    for index in range(shots):
        shot = prepare_shot(bundle, backend, output, index, seed + index)
        summary = execute_shot(simulator, shot)
        exit_code = summary.get("exit_code", 0)
        if exit_code:
            exit_codes[str(exit_code)] += 1
        else:
            counts[decode_result(bundle, shot, summary)] += 1
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
