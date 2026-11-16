#!/usr/bin/env python3
"""Execute the EXP-013 production fixtures and retain their complete, hashed output."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = Path(os.environ['RUNNER_TEMP']) / 'optc-contracts'
out.mkdir(exist_ok=False)
production = [
    'kexts/NVRM/NVRM.cpp', 'kexts/NVRM/os-xnu.cpp', 'kexts/NVRM/nvrm_gpuva.cpp',
    'kexts/NVRM/nvrm_owner_abi.h', 'kexts/NVRM/nvrm_devtab.h',
    'kexts/NMIntelFB/intelfb.cpp', 'kexts/NMIntelFB/intelfb_console.h',
    'plugin/NVMTLDevice.m', 'plugin/NVMTLObjects.m', 'plugin/NVMTLEventSync.m',
    'plugin/nvmtl_vk.c', 'nvk/nvk-macos.patch',
]
production += [str(p.relative_to(root)) for p in (root / 'kexts/NVRM/accel').rglob('*.h')]
production.append('kexts/NVRM/accel/nvrm-accel.cpp')
python_tests = [
    'test_optc_lifetime.py', 'test_optc_console.py', 'test_optc_gpuva.py',
    'test_optc_backend.py', 'test_surface_pending_lifetime.py',
    'test_surface_publication.py', 'test_surface_queue.py', 'test_optc_publication.py',
]
c_tests = [('test_optc_frame_transfer.c', 'cc', 'c11'), ('test_nvrm_devtab.cpp', 'c++', 'c++17')]
paths = sorted(set(production + ['tools/' + n for n in python_tests] + ['tools/' + n for n, _, _ in c_tests] + ['tools/run_optc_contract_tests.py']))
source_files = {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths}
source_commit = os.environ['GITHUB_SHA']
run_id = os.environ['GITHUB_RUN_ID']

def execute(name, command, compile_command=None):
    output = ''
    if compile_command:
        compiled = subprocess.run(compile_command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        output += compiled.stdout
        if compiled.returncode:
            result = compiled
        else:
            result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            output += result.stdout
    else:
        result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        output = result.stdout
    log = out / (name + '.log')
    log.write_text(output)
    sys.stdout.write(output)
    cases = []
    for line in output.splitlines():
        if line.startswith('OPTC_CASE '):
            row = json.loads(line[len('OPTC_CASE '):])
            if not isinstance(row, dict) or not isinstance(row.get('id'), str) or row.get('result') != 'PASS':
                raise ValueError('invalid production case record')
            cases.append(row)
    report = {
        'schema_version': 1,
        'source_commit': source_commit, 'ci_run_id': run_id, 'target_os_build': '25G342',
        'command': command, 'exit_code': result.returncode,
        'log_path': log.name, 'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest(),
        'cases': cases, 'source_files': source_files, 'static_verification_only': True,
    }
    if compile_command:
        report['compile_command'] = compile_command
    (out / (name + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        raise SystemExit(result.returncode)

for name, compiler, standard in c_tests:
    binary = out / Path(name).stem
    compile_command = [compiler, '-std=' + standard, '-O1', '-Wall', '-Wextra', '-Werror',
                       '-Ikexts/NVRM', '-Ikexts/NVRM/accel', '-Ikexts/NMIntelFB',
                       'tools/' + name, '-o', str(binary)]
    execute(Path(name).stem, [str(binary)], compile_command)
for name in python_tests:
    execute(Path(name).stem, ['python3', '-B', 'tools/' + name])
(out / 'source-files.json').write_text(json.dumps(source_files, indent=2) + '\n')
