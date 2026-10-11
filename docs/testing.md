# Test the compiler

Run commands from the compiler repository root after completing the
[build setup](../README.md#build).

```sh
cmake --build --preset clang-ninja --parallel
ctest --preset clang-ninja --output-on-failure
```

List registered tests with `ctest --test-dir build-clang -N`.

## Stage and contract tests

| Tests | Coverage |
| --- | --- |
| `unit.scheduler` | Static cycles, mappings, capacity limits, adaptive resource conflicts and reset waits without LLVM or a simulator. |
| `unit.owner`, `unit.association`, `unit.artifact` | Module ownership, operation bindings after classical instruction insertion and block-view reordering, rejection of changed quantum operations and duplicate IDs, and artifact layout without LLD. |
| `compiler.contracts` | Base QIR validation, bitcode input, ELF properties, output order and artifact contents. |
| `compiler.ideal_target` | Program-derived mappings for static and adaptive QIR, native reset duration, deterministic output, and invalid ideal parameters. |
| `compiler.adaptive` | Accepted control flow, result assignment and rejection of unsupported input. |
| `compiler.artifact_contract` | Packaged schema and ABI agreement with the shared contracts; valid bundles and invalid layouts or field types. |
| `compiler.install` | Installed compiler and Python runner behavior. |
| `architecture.includes`, `architecture.headers` | Permitted module dependencies and independent header compilation. |

## Simulator integration

Install `qsbit-sim` on PATH. For numerical tests, enable its Python bridge and
prepare the [Aer and QEC dependencies](https://github.com/qsbit-org/qsbit-sim/blob/main/docs/backends.md).
Select that environment's interpreter for CTest:

```sh
export QSBIT_TEST_PYTHON="/absolute/path/to/environment/bin/python"
cmake --preset clang-ninja \
  -DPython3_EXECUTABLE="$QSBIT_TEST_PYTHON" \
  -DQSBIT_SIM_EXECUTABLE="$(command -v qsbit-sim)" \
  -DQSBIT_TEST_AER=ON -DQSBIT_TEST_QEC=ON
cmake --build --preset clang-ninja --parallel
ctest --test-dir build-clang -R '^integration\.' --no-tests=error --output-on-failure
```

| Test | Independent checks |
| --- | --- |
| `integration.mock` | Operation timing, measurement-result order and repeated output records. |
| `integration.aer` | Bell-state amplitudes and measurement correlations. |
| `integration.qec` | Measurement loops, correction of each single data-qubit error, decoder reuse, process-order independence and feedback beyond the fixed block interval. |
| `integration.bloq` | Bloq-generated LLVM text and bitcode, conditional decoder waits, phase gates, signed product measurements, bounded retries, repetition-code corrections and surface-code memory. |

### Bloq exporter

Build the `bloq_qir` exporter with Bloq's pinned Rust toolchain and LLVM 21 tools
on PATH, then add it to the configuration above:

```sh
export BLOQ_SOURCE="/absolute/path/to/bloq"
(cd "$BLOQ_SOURCE" && cargo build --locked -p bloq_qir --example export)
export QSBIT_BLOQ_EXPORTER="$BLOQ_SOURCE/target/debug/examples/export"
cmake --preset clang-ninja -DQSBIT_BLOQ_EXPORTER="$QSBIT_BLOQ_EXPORTER"
ctest --test-dir build-clang -R '^integration\.bloq$' --no-tests=error --output-on-failure
```

`integration.bloq` requires both `QSBIT_TEST_QEC` and `QSBIT_TEST_AER`.
It compares surface-code memory results with an independent Stim and PyMatching
reference. Conditional waits and repeat-until-success entry cover both activation
paths and repeated correction queries. Multibit masks exercise bit selection with
higher bits set. Exhausted waits and retries must preserve nonzero program status
and produce no accepted shot.

## Continuous integration

The [CI workflow](../.github/workflows/ci.yml) builds with Clang and GCC, runs
CTest, checks formatting and runs clang-tidy.
The [cross-project workflow](../.github/workflows/integration.yml) builds this
compiler with qsbit-sim main and the Bloq QIR exporter. It runs the integration
tests, installed commands, and the simulator's surface-code and Bloq examples.
The workflow records the revisions it tests.

The [2026-09-20 validation record](validation.md) preserves an earlier environment
and its results.
