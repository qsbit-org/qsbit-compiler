# qsbit-compiler

`qsbitc` compiles supported QIR programs into RISC-V executables for
[qsbit-sim](https://github.com/qsbit-org/qsbit-sim). The first example prepares
and measures a Bell pair using QIR exported by CUDA-Q.

The pipeline is QIR → validated operations → target mapping → timed schedule →
LLVM RISC-V code generation → ELF. LLVM remains unmodified. The compiler uses
C++20 and LLVM 21; Python is used for tests and the simulator runner.

## Build

On Ubuntu 26.04:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 \
  clang-21 llvm-21-dev llvm-21-tools lld-21 clang-format-21 clang-tidy-21
cmake --preset clang-ninja
cmake --build --preset clang-ninja --parallel 4
ctest --preset clang-ninja
```

Use `gcc-ninja` instead of `clang-ninja` to build the host compiler with GCC.
Both choices use LLVM 21 and LLD 21 to generate the RISC-V executable.
Other distributions need equivalent packages, CMake 3.24 or newer, and Ninja.
If several LLVM installations exist, pass `-DLLVM_DIR=/path/to/llvm/lib/cmake/llvm`
to the configure command. The matching LLVM build must include RISC-V support.

## Compile and run Bell

Run these commands from the compiler repository root:

```sh
build-clang/qsbitc examples/bell.ll \
  --target targets/sim-default.json -o out/bell.elf
python3 tools/run.py out/bell.elf \
  --sim ../qsbit-sim/build-clang/qsbit-sim --backend aer --shots 16
```

The simulator must be built with its Python backend enabled and its Aer extra
installed. Follow [the simulator setup](https://github.com/qsbit-org/qsbit-sim/blob/main/docs/backends.md).

Each shot starts a fresh simulator process. Results are saved to
`out/runs/results.json`; each shot also has a run configuration, execution trace,
and summary. Bit strings follow QIR output-record order. For Bell, ideal
measurement results are `00` and `11`; a finite sample need not split equally.
The `mock` backend exercises controller behavior with predetermined results;
use `aer` to simulate the quantum state.

The compiler also writes `bell.lowered.ll`, `bell.schedule.json`,
`bell.manifest.json`, and `bell.run.json`. Keep the artifact bundle together.
The runner checks the ELF digest, result layout, and target profile before
launching the simulator.

## Integration tests

```sh
cmake --preset clang-ninja \
  -DQSBIT_SIM_EXECUTABLE="$(realpath ../qsbit-sim/build-clang/qsbit-sim)" \
  -DQSBIT_TEST_AER=ON
cmake --build --preset clang-ninja --parallel 4
ctest --preset clang-ninja
```

The tests check malformed input, bitcode input, ELF properties, physical action
times, result ordering, repeated output records, profile mismatches, Bell
measurement correlation, and the unmeasured Bell state amplitudes.

## Current scope

The Base Profile subset supports `h`, `x`, `z`, `cnot` or `cx`, and terminal `mz`,
with static qubit and result identifiers. Versioned input must declare QIR 1.0.
A separate compatibility path accepts the unversioned static format emitted by
CUDA-Q 0.16. Input may be LLVM text or bitcode.

Base programs are limited to 16 quantum operations and eight measurements.
Operations run sequentially.

The [Adaptive Profile subset](docs/adaptive.md) supports intermediate
measurements, branches, loops, reset and decoder feedback. Its
[QEC examples](examples/qec/README.md) include a measurement loop and a
three-qubit repetition code. The simulator repository includes a
[distance-3 surface-code example](https://github.com/qsbit-org/qsbit-sim/tree/main/examples/qec).

Qubit placement, routing and dynamic resource allocation are unsupported.
The default target has two qubits; explicit mappings can describe up to 32.
Unsupported instructions produce errors.

See [the implementation contracts](docs/contracts.md),
[CUDA-Q export instructions](integrations/cudaq/README.md), and
[the verified development setup](docs/validation.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
