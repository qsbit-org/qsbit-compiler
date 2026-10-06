# Adaptive QIR

Programs with `qir_profiles="adaptive_profile"` can measure during execution,
branch on results, iterate through loops, and call a decoder. Compile them
with the same `qsbitc INPUT --target TARGET -o OUTPUT.elf` command as Base
Profile programs. The [QEC examples](../examples/qec/README.md) exercise both
local measurement feedback and external decoding.

## Input

The entry point returns void and takes no arguments. It declares
`required_num_qubits` and `required_num_results`. Qubits use static identifiers
from 0 to 31; results use static identifiers within the declared count, which
is at most 65536. Version flags, when supplied, must specify QIR 1.0. Dynamic
resource management must be disabled.

Supported quantum operations are `h`, `x`, `z`, `cx`, `cnot`, `mz` and `reset`.
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
`__quantum__rt__bool_record_output(i1, ptr)`. Labels must be null. Container
records are unsupported. An optional `__quantum__rt__initialize(ptr null)`
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
Reset also reserves this interval after its conditional X. Runtime decoder
polling does not advance the TCU time point.

The executed control-flow path determines the sequence of waits. Different
branches can therefore reach a merge at different time points. Loops retain
their runtime trip count. A command that arrives after its reserved time
fails with `LateAdmission`; the simulator does not move the operation later.
Choose `block_cycles` using the CPU, measurement and decoder delays for the
experiment. These intervals are not a computed worst-case execution-time bound.

## Decoder calls

```llvm
declare void @reset_decoder_ui64(i64 %decoder)
declare void @enqueue_syndromes_ui64(i64 %decoder, i64 %count, i64 %bits, i64 %tag)
declare i64 @get_corrections_ui64(i64 %decoder, i64 %count, i64 %reset)
```

The target's `decoding` object configures the simulator decoder and its MMIO
base. The compiler lowers these calls to ordinary RV32I loads, stores and
polling loops. CPU and TCU instructions remain unchanged.

Enqueue transfers 1–64 bits, least-significant bit first. Decoder IDs and tags
must fit 32 bits. Get waits until all submitted input has completed and returns
the accumulated correction bits; its count must equal the configured decoder
output width. Reset must be 0 or 1. A value of 1 consumes the result. Explicit
reset waits for the decoder to clear its measurement window and corrections.
Invalid arguments trap before the operation proceeds.

The declarations follow the
[CUDA-Q QEC Quantinuum device interface](https://github.com/NVIDIA/cudaq-qec/blob/main/libs/qec/lib/realtime/quantinuum/quantinuum_decoding.h).
They are external functions, not standard QIR instructions. The checked-in
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
requests a memory dump, which `tools/run.py` reads to recover the output stream.
The runner verifies the ELF digest, target profile and decoder configuration.
Each shot starts a new simulator process.

The schedule artifact records `mode: control-flow`, the target start, TCU
period and `block_cycles`. The lowered LLVM IR contains the emitted waits and
branches; the simulator trace records the executed operation times. Adaptive
profiles use a firing width equal to the port count and a 100000000 ns watchdog.

## Tests

`compiler.adaptive` checks accepted control flow, result assignment and rejected
input. Enable `QSBIT_TEST_QEC` with `QSBIT_SIM_EXECUTABLE` to run
`integration.qec`. It checks the measurement loop, correction of each single
data-qubit error, repeated decoder use, process-order independence and late
feedback failure.
