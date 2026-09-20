# Verified development setup

Validated on 2026-09-20 on Linux x86-64, Ubuntu 26.04:

| Component | Version or revision |
| --- | --- |
| LLVM, Clang, LLD | 21.1.8 |
| GCC | 15.2.0 |
| CUDA-Q export package | `cuda-quantum-cu12==0.16.0.post1` |
| CUDA-Q export interpreter | Python 3.12 |
| qsbit-sim | `de58836bcd562c8459309c763a5977ab180dc76d` |
| Simulator Python environment | Python 3.12.14 |
| Qiskit | 2.5.2 |
| Qiskit Aer | 0.17.2 |

The compiler source is at `/mnt/d/qsbit-compiler`. Its Clang build is in
`build-clang`, its GCC build is in `build-gcc`, and its optional CUDA-Q export
environment is in `.venv-cudaq`. These local directories are ignored by Git.
The compiler has no Conan dependency and no temporary SystemC installation.

The simulator is at `/mnt/d/qsbit-sim`. Its SystemC dependency remains in the
Conan cache, with generated dependency files in `.conan/clang/Debug`. Its
Python backend uses the repository's `.venv`; the compiler does not replace
that environment.

To prepare the simulator's optional numerical backend after its normal Conan
setup, run these commands in the simulator repository:

```sh
uv sync --frozen --group build --extra aer --inexact
cmake --preset clang-ninja -DQSBIT_PYTHON_BACKENDS=ON \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset clang-ninja --parallel 4
ctest --test-dir build-clang --output-on-failure --parallel 4
```

The updated simulator passed all 38 configured tests. Its source worktree was
unchanged after the update and validation.

Both compiler presets built successfully and passed the compiler contract
tests. The Clang build additionally passed the scripted integration test and
the Aer integration test. Aer verified the four Bell amplitudes before
measurement against `[1/sqrt(2), 0, 0, 1/sqrt(2)]` with tolerance `1e-10`, then
verified that 16 shots with seeds 1 through 16 produced only `00` and `11`,
with both outcomes present. The saved demo run produced `00` five times and
`11` eleven times; its artifacts are in `out/runs`.

The expected Bell action start times were 10020, 10040, 10060, and 10100 ns.
They matched the simulator's `OperationStart` trace events exactly. These
are configured simulated times, not host execution latency measurements.
The 10000 ns preload interval is a conservative initial compiler setting.

Clang-format 21 and clang-tidy 21 are part of the development checks. The
`.clang-tidy` configuration excludes `clang-analyzer-security.ArrayBound`
because its LLVM 21 operand-storage model reports a false positive inside
`llvm/IR/User.h` when following `CallBase::getCalledFunction()`. Other default
analyzer checks remain enabled and warnings are errors.

GitHub CI builds both compiler presets and runs compiler contract tests.
Numerical integration is currently a local check requiring a separately built
simulator; it is not included in the initial CI workflow.
