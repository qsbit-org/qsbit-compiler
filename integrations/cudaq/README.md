# Export the Bell fixture with CUDA-Q

`examples/bell.ll` was exported with CUDA-Q 0.16.0, from package
`cuda-quantum-cu12==0.16.0.post1`. The reported CUDA-Q source revision was
`62fce8b862302158e0119c65e3ce57538f3af048`.

For CPU-only QIR export, the following minimal environment was tested on Linux
x86-64 with Python 3.12. It omits the wheel's GPU runtime dependencies and is
not an installation recipe for GPU execution.

```sh
uv venv --python 3.12 .venv-cudaq
uv pip install --python .venv-cudaq/bin/python --no-deps \
  cuda-quantum-cu12==0.16.0.post1
uv pip install --python .venv-cudaq/bin/python astpretty numpy scipy requests
.venv-cudaq/bin/python integrations/cudaq/export_bell.py out/cudaq-bell.ll
build-clang/qsbitc out/cudaq-bell.ll \
  --target targets/sim-default.json -o out/cudaq-bell.elf
```

The exporter selects `qpp-cpu` and calls `cudaq.translate` with `qir-base`.
CUDA-Q includes a process-dependent suffix in the entry name, so repeated
exports need not be byte-identical. The compiler identifies the entry by its
attribute, not its name. The committed fixture is the exported QIR, including
its host data layout and resource attributes.

This exporter emits `requiredQubits` and `requiredResults` and does not declare
a QIR module version. The compiler records this input as
`cudaq-static-base-unversioned`. Dynamic CUDA-Q kernels are outside the current
compiler subset.
