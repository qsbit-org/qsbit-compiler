# Measurement feedback and error correction

Install `qsbitc` and `qsbit-run` using the [build instructions](../../README.md#build).
Run these commands from the compiler repository root. Install qsbit-sim with
Python support and install its `qec` extra as described in the
[simulator example](https://github.com/qsbit-org/qsbit-sim/tree/main/examples/qec).

## Reuse a measurement result in a loop

```sh
qsbitc examples/qec/feedback.ll \
  --target targets/sim-default.json -o out/qec/feedback.elf
qsbit-run out/qec/feedback.elf \
  --backend stim --shots 1 \
  --out-dir out/qec/feedback
```

The program prepares qubit 0 in state 1, measures it three times, and applies X
when the result is 1. It reuses QIR result 0 on each iteration. The output is
`100` in `out/qec/feedback/results.json`.

## Correct a bit flip

```sh
qsbitc examples/qec/repetition.ll \
  --target examples/qec/repetition.target.json -o out/qec/repetition.elf
qsbit-run out/qec/repetition.elf \
  --backend stim --shots 1 \
  --out-dir out/qec/repetition
```

The three data qubits begin in `000`. An X gate introduces an error on data
qubit 1. Two ancillas measure the Z parities of data pairs (0, 1) and (1, 2).
The program sends the parity bits to PyMatching and applies the returned X
corrections before measuring the data qubits. It repeats this process three
times, resetting both ancillas before each round.

Each round records two parity bits followed by three data bits. The expected
output is `11000 00000 00000`, stored without spaces as `110000000000000`.
The first round detects and corrects the injected error; the next two rounds
have zero parity and data bits.

`repetition.target.json` sets the link delay, transfer bandwidth, queue
capacities, decoder latency and initiation interval. Increase decoder
`latency` to `100000` to exercise a delay beyond the fixed block interval.
The generated `wait 0` instructions let the TCU pause while the CPU polls
the decoder. The expected output bits stay unchanged; the stop tick increases.

See [Adaptive QIR](../../docs/adaptive.md) for supported instructions, output
layout and decoder calls.
