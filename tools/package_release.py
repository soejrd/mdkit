#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Package standalone voice sources and required headers, without player/build files."""
import argparse
from pathlib import Path
import re
import zipfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('version', help='Release version without the v prefix, e.g. 1.0')
args = parser.parse_args()
if not re.fullmatch(r'[0-9]+\.[0-9]+(?:\.[0-9]+)?', args.version):
    parser.error('version must have the form 1.0 or 1.0.0')
files = sorted(p for p in (root / 'src').rglob('*')
               if p.is_file() and p.suffix in {'.c', '.h'})
files += sorted((root / 'include').glob('*.h'))
files += [root / 'LICENSE', root / 'NOTICE']
if len([p for p in files if p.suffix == '.c']) != 30:
    raise SystemExit('Expected exactly thirty voice implementations')
output = root / 'out' / 'release' / f'mdkit-{args.version}-src.zip'
output.parent.mkdir(parents=True, exist_ok=True)
prefix = f'mdkit-{args.version}/'
readme = f'''# mdkit {args.version} — voice sources

Thirty standalone C99 voices: EFM, TRX and PI.

Compile the chosen src/<family>/<voice>/mdkit_<family>_<voice>.c file and
include its matching header. Add include/ to your compiler's header search
path. Each instance needs its own caller-owned state. Use the voice descriptor's
init, set_control, trigger and render operations. Controls range from 0 to 127;
output is mono float32 at 44,100 Hz. Initialize once and retain state between
render calls. Initialization restores the voice's default control settings.

The source repository contains usage examples, checks and the showcase player:
https://github.com/soejrd/mdkit

See LICENSE and NOTICE for CC0 1.0 Universal terms and scope.
'''
with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    def add(name, content):
        info = zipfile.ZipInfo(prefix + name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, content)
    for path in files:
        add(path.relative_to(root).as_posix(), path.read_bytes())
    add('README.md', readme.encode())
print(output)
