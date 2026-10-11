"""Ideal target generation uses validated QIR, including native reset operations."""
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

compiler, root = map(Path, sys.argv[1:])
source = (root / "examples/qec/feedback.ll").read_text()
source = 'declare void @__quantum__qis__reset__body(ptr)\n' + source.replace(
    'entry:\n', 'entry:\n  call void @__quantum__qis__reset__body(ptr null)\n')
preset = {
    "schema": 1, "name": "test-ideal", "start_ns": 10000,
    "cpu_period_ns": 10, "tcu_period_ns": 20, "block_cycles": 500,
    "ideal": {"max_qubits": 2, "gate_duration_ns": 60,
              "measurement_duration_ns": 80, "reset_duration_ns": 100},
}
with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    qir, target, elf = work / 'program.ll', work / 'target.json', work / 'program.elf'
    qir.write_text(source)

    def compile_case(config, diagnostic=None):
        target.write_text(json.dumps(config))
        result = subprocess.run([compiler, qir, '--target', target, '-o', elf],
                                capture_output=True, text=True, check=False)
        if diagnostic:
            assert result.returncode != 0 and diagnostic in result.stderr, result.stderr
        else:
            assert result.returncode == 0, result.stderr
            return json.loads(elf.with_suffix('.run.json').read_text())

    run = compile_case(preset)
    profile = run['profile']
    assert profile['qubits'] == profile['ports'] == 1
    assert profile['cpu']['period'] == 10
    actions = [a for m in profile['mappings'] for a in m['actions']]
    for operation, duration in [('reset', 100), ('x', 60), ('measure', 80)]:
        assert any(a['operation'] == operation and a['duration'] == duration for a in actions)
    assert all(a['targets'] == [0] for a in actions)
    assert compile_case(preset) == run
    bad = copy.deepcopy(preset)
    bad['mappings'] = []
    compile_case(bad, 'cannot be combined')
    for field, value in [('max_qubits', 33), ('gate_duration_ns', 0), ('unexpected', 1)]:
        bad = copy.deepcopy(preset)
        bad['ideal'][field] = value
        compile_case(bad, 'target:')
    # Static QIR follows the same target generation contract.
    qir.write_text((root / 'examples/bell.ll').read_text())
    static = compile_case(preset)['profile']
    assert static['qubits'] == static['ports'] == 2
    assert any(a['operation'] == 'cx' and a['targets'] == [0, 1]
               for m in static['mappings'] for a in m['actions'])
    qir.write_text(source.replace('"required_num_qubits"="1"', '"required_num_qubits"="3"'))
    compile_case(preset, 'qubit allocation')
print('Ideal target generation and validation passed.')
