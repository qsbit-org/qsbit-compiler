# Build and install

Run commands from the compiler repository root.

## Prerequisites

On Ubuntu 26.04:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build python3 python3-jsonschema git \
  clang-21 llvm-21-dev llvm-21-tools lld-21 clang-format-21 clang-tidy-21
```

Other distributions need equivalent packages, CMake 3.24 or newer, and Ninja.
LLVM 21 must include RISC-V support. LLVM and LLD 21 remain runtime dependencies.

CMake uses an installed controller contract package or downloads its pinned
revision. See [controller contracts](contracts.md#controller-contract-package)
for local and offline builds.

## Native compiler

```sh
cmake --preset clang-ninja
cmake --build --preset clang-ninja --parallel
cmake --install build-clang --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

Add the PATH setting to your shell configuration. Use `gcc-ninja` instead of
`clang-ninja` to build the host compiler with GCC. Both presets use LLVM 21 and
LLD 21 to generate RISC-V executables. If several LLVM installations exist,
pass `-DLLVM_DIR=/path/to/llvm/lib/cmake/llvm` to the configure command.

## Simulator runner

Install the Python runner with [uv](https://docs.astral.sh/uv/):

```sh
uv tool install .
uv tool update-shell
```

Reopen your shell if its configuration changed. This installs `qsbit-run`;
CMake installs `qsbitc`. The runner requires an installed
[qsbit-sim](https://github.com/qsbit-org/qsbit-sim#build) on PATH, or an executable
selected with `--sim PATH`. Numerical backends require the simulator's Python
bridge and [backend dependencies](https://github.com/qsbit-org/qsbit-sim/blob/main/docs/backends.md).

Return to the [Bell example](../README.md#compile) or run the
[compiler tests](testing.md).
