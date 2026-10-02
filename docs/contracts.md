# Compiler contracts

## Modules

- `lib/QIR` parses and verifies LLVM IR, identifies the entry point, validates
  the supported QIR subset, and collects operations and output records.
- `lib/Target` reads explicit operation mappings and constructs the complete
  simulator device profile.
- `lib/Scheduling` assigns each operation a controller cycle.
- `lib/CodeGen` lowers the schedule into LLVM IR, emits an RV32I object, links
  it with LLD, and validates the resulting ELF.
- `tools/run.py` validates the artifact bundle and launches independent shots.

The normalized program and schedule are C++ structures in
`include/qsbit/Compiler.hpp`. They are internal APIs, not a stable external IR.
Adding an input framework should produce accepted QIR or introduce a separate
frontend. Target mappings describe physical execution independently of the
frontend's Python classes.

## QIR input

Exactly one defined function must have `entry_point` and
`qir_profiles="base_profile"`. It must return void, take no arguments, and
contain a single basic block. Helper function definitions, module assembly,
classical instructions, indirect calls, and unsupported runtime calls are
rejected. The parser does not eliminate dead code or lower structured control
flow before checking these constraints.

Static counts use `required_num_qubits` and `required_num_results`, or the CUDA-Q
spellings `requiredQubits` and `requiredResults`. Counts must be between zero and
64; conflicting spellings are rejected. Static identifiers use null for zero
or a constant integer-to-pointer expression and must fit the declared counts.

Versioned modules must declare QIR major version 1 and minor version 0.
Dynamic resource management flags, when present, must be false. Unversioned
input requires CUDA-Q's qubit-count attribute and uses a separate dialect name
in the manifest. This is a compatibility rule for the tested exporter, not a
claim of support for all CUDA-Q programs.

Supported direct calls are:

| Call suffix | Meaning |
| --- | --- |
| `__quantum__qis__h__body` | Hadamard on one qubit |
| `__quantum__qis__x__body` | Pauli X on one qubit |
| `__quantum__qis__z__body` | Pauli Z on one qubit |
| `__quantum__qis__cnot__body` or `__quantum__qis__cx__body` | Controlled X on distinct control and target qubits |
| `__quantum__qis__mz__body` | Z measurement into a static result identifier |
| `__quantum__rt__initialize` | Optional first call, with a null argument |
| `__quantum__rt__result_record_output` | Append one measured result to the output stream |
| `__quantum__rt__array_record_output` | Begin an array with a static element count |
| `__quantum__rt__tuple_record_output` | Begin a tuple with a static element count |

All gates precede all measurements. A qubit is measured at most once, and a
result identifier is assigned at most once. Output records follow quantum
operations. They may repeat a result or change the order of results. Containers
must have their declared number of children. Labels must be constant strings
or null. Limits are 128 output records, 64 elements per container, and 16 levels
of nesting. The manifest preserves container records and labels; the runner
reports a flat bit string in leaf-record order.

## Target description

`targets/sim-default.json` is the initial target description. Schema 1 accepts
only these top-level keys: `schema`, `name`, `qubits`, `ports`, `start_ns`,
and `mappings`. Unknown keys are rejected.

Each mapping contains `operation`, ordered `qubits`, `port`, `codeword`, and
`duration_ns`. A port is a controller output endpoint, not a qubit identifier.
A CX mapping specifies both participating qubits even though one port receives
one codeword. There is no implicit connectivity or routing. Both the operation
with its ordered qubits and the port with its codeword must be unique.

Target limits are 1–32 qubits, 1–64 ports, codewords 0–65535, and operation
durations 1–10000 ns. `start_ns` is between 10000 and 100000 ns and must align
to 20 ns. Each mapping expands to one simulator action with no action delay,
exclusive resources for its qubits, and a 20 ns discriminator delay.

This target format fixes the CPU clock at 5 ns and the timing control unit
(TCU) clock at 20 ns. The profile uses 32 timing entries, 32 event entries,
16 staging entries, eight outstanding measurements per delivery path, and
firing width one. The manifest embeds the target profile and transport latencies.

## Scheduling and controller instructions

The first operation starts at TCU cycle 1. For an operation of duration `d` ns,
the next operation starts `ceil(d / 20)` cycles later. The absolute start time
is `start_ns + cycle * 20` ns. Independent gates are also serialized.

The CPU preloads the complete schedule before the target start time. The
16-operation bound fits the fixed timing and event queues. Measurements are
limited to eight outstanding result deliveries. ELF validation requires
the preload instruction count multiplied by 100 ns to be strictly less
than `start_ns`.

The compiler emits control instructions with opcode `0x0b`, funct7 zero
and these funct3 values:

| Value | Instruction | Use |
| --- | --- | --- |
| 0 | `cw.r.r` | Prepare events using port and codeword GPRs; rd is zero. |
| 1 | `wait.r` | Advance the time point by the interval in rs1; rd and rs2 are zero. |
| 3 | `FMR` | Copy a measurement register into rd; rs1 holds the qubit index and rs2 is zero. |

LLVM emits these instructions through side-effecting inline assembly with
memory clobbers. FMR waits for the selected qubit's pending measurements and
leaves its result register unchanged. The compiler emits one FMR per measured
qubit; repeated output records reuse the captured bit.

The program exits with `ECALL`, `a7 = 93` and `a0 = 0`. The simulator enqueues
pending events and waits for device work and result deliveries to finish.

## Executable and artifacts

The ABI is `qsbit-static-v2`: little-endian ELF32, RISC-V machine type, entry
address zero, no compressed instructions, and ELF flags zero. The startup
sets the stack pointer to `0xfff0`. Executable text must fit below `0x1000`.
Output bits occupy consecutive 32-bit words beginning at `0x1000`. The target
requires at least 64 KiB of simulator RAM. No hosted C library is linked.

The compiler emits a RISC-V LLVM module. LLD links the object with an internal
linker script.
Compilation and validation take place in a temporary directory under the output
directory. Files are published only after validation; publication of the whole
bundle is not an atomic filesystem transaction. The runner's digest and profile
checks detect mismatched executable and configuration files.

The manifest stores input dialect, entry attributes, output records, target
profile, ELF SHA-256, and ABI identifiers. The schedule stores expected action
start times. The run configuration embeds the same profile and result addresses.
The runner verifies equality before execution and changes only the random seed
within the profile between shots. These checks detect accidental mismatch;
they do not authenticate artifacts from untrusted sources.
