# Adaptive QIR

Programs with `qir_profiles="adaptive_profile"` can measure during execution,
branch on results, iterate through loops, and call a decoder. Compile them
with the same `qsbitc INPUT --target TARGET -o OUTPUT.elf` command as Base
Profile programs. The [QEC examples](../examples/qec/README.md) exercise both
local measurement feedback and external decoding.

## Input

The entry point takes no arguments. QIR 2.1 entries return an `i64` program
status; existing QIR 1.0 entries may return void or `i64`. It declares
`required_num_qubits` and `required_num_results`. Qubits use static identifiers
from 0 to 31; results use static identifiers within the declared count, which
is at most 65536. Version flags, when supplied, must specify QIR 1.0 or 2.1.
Dynamic resource management must be disabled.

Supported quantum operations are `h`, `x`, `z`, `s`, `s` adjoint, `t`,
`t` adjoint, `cx`, `cnot`, `mz` and `reset`. Target operation names for
the phase gates are `s`, `sdg`, `t` and `tdg`; non-Clifford gates require
a compatible simulator backend, such as Aer.
Reset emits a measurement followed by X when the measurement is 1.
`__quantum__rt__read_result` and `__quantum__qis__read_result__body` return `i1`.
Every result read must follow an assignment on all incoming control-flow paths.
Each `mz` emits FMR and stores the bit in the corresponding result slot before
execution continues. Reassigning a slot replaces that bit; previously computed
SSA values retain their values.

Classical code supports conditional and unconditional branches, returns, phi
nodes, comparisons, select, integer extension and truncation, addition,
subtraction, bitwise operations and shifts. Integer widths are at most 64 bits.
Acyclic direct helper calls are inlined. Recursion, indirect calls, mutable
input globals, local memory operations, floating-point arithmetic and
unrecognized calls produce errors.

Output uses `__quantum__rt__result_record_output(ptr, ptr)` or
`__quantum__rt__bool_record_output(i1, ptr)`. Labels may be null or constant
strings. The flat output buffer preserves call order; label names are not stored.
Container records are unsupported. An optional `__quantum__rt__initialize(ptr null)`
requires no runtime action.

## Timing

The first quantum operation uses the target's `start_ns`. Within a basic block,
up to 16 operations with disjoint qubits and output ports share a time point.
A conflict or a full staging buffer
advances the time point by the longest operation duration at that point,
rounded up to TCU cycles.

`block_cycles` is a target setting from 1 to 1000000, with default 1000. After
each measurement and at each basic-block exit, the compiler emits a wait of
at least `block_cycles`. Any unfinished operation duration extends that wait.
Reset also reserves this interval after its conditional X.

Before each measurement read and decoder call, the compiler emits `wait.r x0`.
After this zero wait triggers, an empty timing queue pauses the TCU logical
timer while the CPU waits for the result. CPU instructions, decoder transport
and quantum-state evolution continue in physical time. Subsequent work resumes
the TCU; a positive-interval point restores strict deadlines when it triggers.

The executed control-flow path determines the sequence of waits. Different
branches can therefore reach a merge at different time points. Loops retain
their runtime trip count. Outside an explicit zero-wait region, a command that
arrives after its reserved time fails with `LateAdmission`. Choose `block_cycles`
to cover CPU issue time between feedback waits. It need not bound measurement
or decoder latency protected by `wait 0`.

## Decoder calls

```llvm
declare void @reset_decoder_ui64(i64 %decoder)
declare void @enqueue_syndromes_ui64(i64 %decoder, i64 %count, i64 %bits, i64 %tag)
declare i64 @get_corrections_ui64(i64 %decoder, i64 %count, i64 %reset)
declare i1 @decoder_ready_ui64(i64 %decoder)
```

The target's `decoding` object configures the simulator decoder and its MMIO
base. The compiler lowers these calls to ordinary RV32I loads, stores and
polling loops preceded by `wait 0`.

Enqueue transfers 1–64 bits, least-significant bit first. Decoder IDs and tags
must fit 32 bits. Get waits until all submitted input has completed and returns
the accumulated correction bits; its count must equal the configured decoder
output width. Reset must be 0 or 1. A value of 1 consumes the result. Explicit
reset waits for the decoder to clear its measurement window and corrections.
Invalid arguments trap before the operation proceeds.

`decoder_ready_ui64` performs one status read and returns true only when
an unread result exists and no work remains on the selected session. It
neither waits for decoding nor consumes corrections. A QIR branch can execute
a protection circuit and poll again while false. The existing get call
remains blocking even when its consumption argument is zero.

The first three declarations follow the
[CUDA-Q QEC Quantinuum device interface](https://github.com/NVIDIA/cudaq-qec/blob/main/libs/qec/lib/realtime/quantinuum/quantinuum_decoding.h).
All four are external functions, not standard QIR instructions.
The readiness query is a local extension of the existing decoder interface. The checked-in
examples supply these declarations directly. CUDA-Q's complete QEC export
pipeline has not been validated against this compiler.

For transport parameters and window semantics, see the
[simulator decoder contract](https://github.com/qsbit-org/qsbit-sim/blob/main/docs/decoding.md).

## Artifacts

The executable ABI is `qsbit-adaptive-v1`. RAM occupies the first 1 MiB,
the stack starts at `0xffff0`, text stays below `0x10000`, and data starts at
`0x20000`. A word at `0x10000` holds the output count. Up to 16383 output bits
follow at `0x10004`, each in a little-endian 32-bit word. Output overflow traps.

The manifest records this layout and the decoder configuration. The run file
requests a memory dump, which `qsbit-run` reads to recover the output stream.
The runner verifies the ELF digest, target profile and decoder configuration.
Each shot starts a new simulator process.

The entry status is returned through the exit ECALL in register `a0`.
The simulator reports it as `exit_code` independently of architectural faults.
The runner includes only status-zero shots in `counts`; nonzero program
statuses are counted separately in `exit_codes`. The controller retains
32 status bits, so this subset requires statuses in the range 0–63.

The schedule artifact records `mode: control-flow`, the target start, TCU
period and `block_cycles`. The lowered LLVM IR contains the emitted waits and
branches; the simulator trace records the executed operation times. Adaptive
profiles use a firing width equal to the port count and a 100000000 ns watchdog.

## Tests

`compiler.adaptive` checks accepted control flow, result assignment and rejected
input. Enable `QSBIT_TEST_QEC` with `QSBIT_SIM_EXECUTABLE` to run
`integration.qec`. It checks the measurement loop, correction of each single
data-qubit error, repeated decoder use, process-order independence and feedback
with decoder latency exceeding the fixed block interval.

For Bloq integration, build `bloq_qir`'s `export` example in the Bloq workspace,
then configure this project with `QSBIT_BLOQ_EXPORTER` pointing to that executable
and both `QSBIT_TEST_QEC=ON` and `QSBIT_TEST_AER=ON`. Select a Python
interpreter with the QEC and Aer extras installed. Registered test `integration.bloq`
runs the Rust exporter, compiles both LLVM text and bitcode, and executes
measurement feedback, phase gates, signed product measurements, bounded retries,
all three repetition-code errors, protection during decoder latency, and a
Bloq-compiled d3 X-memory with an independent Stim/PyMatching reference.
Retry and wait exhaustion must preserve nonzero status and yield no accepted shot.
