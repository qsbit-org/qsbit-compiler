"""Validate executable bundles against their manifest and ABI."""

import argparse
import hashlib
import json
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


def main():
    parser = argparse.ArgumentParser(description="Validate a qsbit executable bundle.")
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    try:
        load_bundle(args.elf)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
