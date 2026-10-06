#!/usr/bin/env python3
"""Record target identity and hashes of deliverable SMIT binaries/packages."""
import hashlib
import json
from pathlib import Path
import struct
import sys


def manifest(directory, platform, kernel):
    directory = Path(directory)
    expected = 8 if platform == 'openwrt-mt7621' else 62  # ELF e_machine: MIPS / x86_64
    files = [directory / 'smit', directory / 'smit.ko']
    files += sorted(directory.glob('*.apk'))
    files += sorted((directory / 'dependencies').glob('*.ko'))
    entries = []
    for path in files:
        data = path.read_bytes()
        if path.suffix != '.apk':
            if data[:4] != b'\x7fELF' or data[5] not in (1, 2):
                raise ValueError(f'Not ELF: {path}')
            machine = struct.unpack_from('<H' if data[5] == 1 else '>H', data, 18)[0]
            if machine != expected:
                raise ValueError(f'Wrong ELF target {machine}: {path}')
        entries.append({'file': path.relative_to(directory).as_posix(),
                        'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    report = {'product': 'SMIT iCast USB-C/DTMB USB Tuner',
              'platform': platform, 'kernel': kernel, 'artifacts': entries}
    (directory / 'manifest.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    (directory / 'SHA256SUMS').write_text(''.join(f"{entry['sha256']}  {entry['file']}\n" for entry in entries), encoding='utf-8')


if __name__ == '__main__':
    manifest(*sys.argv[1:])
