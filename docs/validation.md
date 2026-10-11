# Historical validation: 2026-09-20

This record describes validation on Linux x86-64 with Ubuntu 26.04 on
2026-09-20. For current instructions, see [building](../README.md#build) and
[testing](testing.md).

## Recorded environment

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

## Recorded results

Both compiler presets built successfully and passed the compiler contract
tests. The Clang build additionally passed the mock integration test and
the Aer integration test. Aer verified the four Bell amplitudes before
measurement against `[1/sqrt(2), 0, 0, 1/sqrt(2)]` with tolerance `1e-10`, then
verified that 16 shots with seeds 1 through 16 produced only `00` and `11`,
with both outcomes present. The saved demo run produced `00` five times and
`11` eleven times; its artifacts are in `out/runs`.

The expected Bell action start times were 10020, 10040, 10060, and 10100 ns.
They matched the simulator's `OperationStart` trace events exactly. These
are configured simulated times, not host execution latency measurements.
The 10000 ns preload interval is a conservative initial compiler setting.

Clang-format 21 and clang-tidy 21 were part of the development checks. The
`.clang-tidy` configuration excluded `clang-analyzer-security.ArrayBound`
because its LLVM 21 operand-storage model reported a false positive inside
`llvm/IR/User.h` when following `CallBase::getCalledFunction()`. Other default
analyzer checks remained enabled, with warnings treated as errors.

At the time of this validation, CI built both compiler presets and ran compiler
contract tests. Numerical integration ran locally with a separately built simulator.
