"""Export a Bell kernel using CUDA-Q's static QIR Base Profile lowering."""
import argparse
from pathlib import Path
import cudaq


@cudaq.kernel
def bell():
    qubits = cudaq.qvector(2)
    h(qubits[0])
    x.ctrl(qubits[0], qubits[1])
    mz(qubits)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    cudaq.set_target("qpp-cpu")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(cudaq.translate(bell, format="qir-base"))
    print(f"Exported CUDA-Q {cudaq.__version__} Bell QIR to {args.output}")


if __name__ == "__main__":
    main()
