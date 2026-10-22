#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Run every PI contract check and native/WASM comparison, continuing on failure."""
from pathlib import Path
import argparse
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--voices', default='bd,sd,mt,ml,ma,rs,rc,cc,hh')
a = p.parse_args()
failures = []
out = ROOT / 'out/pi-suite'
out.mkdir(parents=True, exist_ok=True)

def run(command, label):
    result = subprocess.run([str(x) for x in command], cwd=ROOT)
    if result.returncode:
        failures.append(label)
    return result.returncode == 0

run(['make', 'out/pi_headers'], 'public headers')
for voice in a.voices.split(','):
    if voice not in {'bd','sd','mt','ml','ma','rs','rc','cc','hh'}:
        raise SystemExit('Unknown PI voice: ' + voice)
    print(f'=== PI-{voice.upper()} ===', flush=True)
    key = 'pi_' + voice
    if not run(['make', f'out/{key}/test_voice', f'out/{key}/voice.wasm'], key + ' build'):
        continue
    raw = out / (voice + '.raw')
    if run([ROOT / f'out/{key}/test_voice', '--render', raw], key + ' native'):
        run(['node', ROOT / 'tests/pi/check_parity.cjs', ROOT / f'out/{key}/voice.wasm',
             raw, 'mdkit_' + key], key + ' native/WASM parity')
print('PI failures: ' + (', '.join(failures) if failures else 'none'))
sys.exit(bool(failures))
