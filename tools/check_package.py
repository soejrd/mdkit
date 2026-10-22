#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Check the copyable source package and its complete voice roster."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--require-all', action='store_true')
a = p.parse_args()
expected = {
    'efm': 'bd sd xt cp rs cb hh cy'.split(),
    'trx': 'bd sd xt cp rs cb ch oh cy ma cl xc b2'.split(),
    'pi': 'bd sd mt ml ma rs rc cc hh'.split(),
}
wanted = {f'{family}/{voice}' for family, voices in expected.items() for voice in voices}
found = {str(path.parent.relative_to(root / 'src')) for path in (root / 'src').glob('*/*/voice.mk')}
errors = []
if a.require_all and found != wanted:
    errors.append('Voice roster incomplete; missing: ' + ', '.join(sorted(wanted - found)))
if found - wanted:
    errors.append('Unexpected voices: ' + ', '.join(sorted(found - wanted)))
for voice in found:
    key = voice.replace('/', '_')
    for suffix in ('.c', '.h'):
        if not (root / 'src' / voice / f'mdkit_{key}{suffix}').is_file():
            errors.append(f'Missing source/header for {voice}')
required = ['README.md', 'LICENSE', 'NOTICE', 'Makefile', 'website/index.html',
            'website/mdkit.wasm', 'website/script.js', 'website/style.css',
            'website/machine-worklet.js', 'website/sample-worker.js', 'tools/generate_catalog.py',
            '.github/workflows/pages.yml']
for file in required:
    if not (root / file).is_file():
        errors.append('Missing package file: ' + file)
files = []
allowed_roots = {'src', 'include', 'examples', 'tests', 'website', 'tools'}
allowed_top = {'README.md', 'LICENSE', 'NOTICE', 'Makefile', '.gitignore'}
for path in sorted(root.rglob('*')):
    rel = path.relative_to(root)
    if rel.parts[0] in {'out', '.git'} or '__pycache__' in rel.parts:
        continue
    if path.is_symlink():
        errors.append(f'Symlink outside copy contract: {rel}')
    if not path.is_file():
        continue
    files.append(str(rel))
    if (rel.parts[0] not in allowed_roots and str(rel) not in allowed_top
            and str(rel) not in {'.github/workflows/pages.yml', '.github/workflows/release.yml'}):
        errors.append(f'Unexpected package entry: {rel}')
    if path.suffix in {'.bin', '.rom', '.raw', '.wav', '.img', '.syx'}:
        errors.append(f'Unexpected data asset: {rel}')
    if path.suffix in {'.c', '.h', '.js', '.html', '.css', '.md', '.py', '.cjs'}:
        text = path.read_text()
        # Patterns are assembled here so this check does not match its own source.
        forbidden = ['md' + 'decomp', 'gear' + 'mulator', 'DSP' + '56',
                     'Pseudo' + ' voice', '/Users' + '/', 'trx-' + 'kernels.bin',
                     'efm-bd-' + 'controls.bin']
        for marker in forbidden:
            if marker.lower() in text.lower():
                errors.append(f'Private dependency/history marker in {rel}: {marker}')
    if path.suffix in {'.c', '.h'}:
        for include in re.findall(r'^\s*#include\s*"([^"]+)"', path.read_text(), re.M):
            candidates = [path.parent / include, root / 'include' / include, root / 'out' / include] + list((root / 'src').glob('*/*/' + include))
            if not any(candidate.is_file() and candidate.resolve().is_relative_to(root) for candidate in candidates):
                errors.append(f'Unresolved local include {include} in {rel}')
module = root / 'website/mdkit.wasm'
if module.is_file():
    check_module = """
const fs = require('fs');
const module = new WebAssembly.Module(fs.readFileSync(process.argv[1]));
if (WebAssembly.Module.imports(module).length) throw new Error('Player has unexpected host imports');
const wasm = new WebAssembly.Instance(module, {}).exports;
const actual = wasm.mdkit_web_machine_count();
const expected = Number(process.argv[2]);
if (actual !== expected) throw new Error(`Player contains ${actual} voices, but ${expected} are registered`);
console.log(`Player catalog: ${actual} voices, no imports`);
"""
    try:
        result = subprocess.run(['node', '-e', check_module, str(module), str(len(found))],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            detail = next((line.strip() for line in result.stderr.splitlines()
                           if line.startswith('Error:')), 'Could not verify compiled player')
            errors.append(detail)
        else:
            print(result.stdout.strip())
    except (OSError, subprocess.TimeoutExpired) as error:
        errors.append('Could not verify compiled player: ' + str(error))
print(f'{len(found)}/30 registered voices; {len(files)} copyable files (excluding out/ and Python caches)')
print('Missing voices: ' + (', '.join(sorted(wanted - found)) or 'none'))
for error in errors:
    print('FAIL: ' + error, file=sys.stderr)
if errors:
    raise SystemExit(1)
print('Package contents and local dependencies OK')
