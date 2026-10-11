# QSBIT compiler

[![CI](https://github.com/qsbit-org/qsbit-compiler/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/qsbit-org/qsbit-compiler/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qsbit-org/qsbit-compiler)](LICENSE)

qsbit-compiler compiles QIR programs into RV32I executables for
[qsbit-sim](https://github.com/qsbit-org/qsbit-sim). It validates input, maps
quantum operations to the target, schedules them, and generates code using LLVM 21.

[Supported QIR](docs/contracts.md#base-profile-input) ·
[Adaptive QIR](docs/adaptive.md) ·
[QEC examples](examples/qec/README.md)

## Build

Install the [prerequisites](docs/building.md), then run from the repository root:

```sh
cmake --preset clang-ninja
cmake --build --preset clang-ninja --parallel
cmake --install build-clang --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

Install the Python runner with [uv](https://docs.astral.sh/uv/):

```sh
uv tool install .
uv tool update-shell
```

Reopen your shell if its configuration changed. Install
[qsbit-sim](https://github.com/qsbit-org/qsbit-sim#build) on PATH before running
compiled programs.

## Compile

Compile the Bell-pair example from the repository root:

```sh
qsbitc examples/bell.ll --target targets/sim-default.json -o out/bell.elf
```

The compiler writes the ELF, lowered LLVM IR, schedule, manifest, and run
configuration under `out/`. Keep these files together.

## Run

```sh
qsbit-run out/bell.elf --backend mock --shots 1
```

The mock backend supplies predetermined measurement bits. The runner saves
results to `out/runs/results.json`, with a trace and summary for each shot.
Use `--sim PATH` to select a different simulator executable.

For quantum-state evolution, configure the simulator's
[Aer backend](https://github.com/qsbit-org/qsbit-sim/blob/main/docs/backends.md), then run:

```sh
qsbit-run out/bell.elf --backend aer --shots 16
```

Each shot starts a fresh simulator process. Ideal Bell measurements produce
`00` or `11`; a finite sample need not split equally.

## Supported input

The compiler accepts LLVM text and bitcode implementing the supported Base or
Adaptive QIR subset. Base programs use static resources and terminal measurements;
Adaptive programs can use intermediate measurements, branches, loops, reset, and
decoder feedback. Unsupported instructions produce errors. Qubit placement,
routing, and dynamic resource allocation are not supported.

## Learn more

- [Build setup](docs/building.md): dependencies and compiler selection.
- [Compiler contracts](docs/contracts.md): modules, QIR constraints, targets, and artifacts.
- [Adaptive QIR](docs/adaptive.md): control flow and decoder calls.
- [CUDA-Q export](integrations/cudaq/README.md): generate QIR input.
- [Testing](docs/testing.md): stage checks and simulator integration.

## License

[Apache License 2.0](LICENSE).
