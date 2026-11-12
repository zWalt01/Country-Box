#!/usr/bin/env python3
# Country-Box: writes games/<game>/manifest.toml, the list the launcher checks a player's extracted copy against
# (launcher/src/manifest.cpp). Only facts measured from a retail dump go in: the title id read from exefs/main.npdm,
# and every file's relative path, size and SHA-256. No game data.
#
#   python3 launcher/make_manifest.py --game dkcrhd --copy /path/to/extracted   (the folder holding exefs/ and romfs/)
import argparse
import datetime
import hashlib
import os
import struct
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMES = {
    # id: (name, title id, display version, build_linux.py version key)
    'dkcrhd': ('Donkey Kong Country Returns HD', '01009D901BC56000', '1.0.0', 'v0'),
    'dkctf': ('Donkey Kong Country: Tropical Freeze', '0100C1F0051B6000', '1.0.0', 'v0'),
}


def title_id(npdm):
    b = npdm.read_bytes()
    aci, = struct.unpack_from('<I', b, 0x70)
    if b[:4] != b'META' or b[aci:aci + 4] != b'ACI0':
        sys.exit(f'{npdm}: not an NPDM with an ACI0 section')
    return '%016X' % struct.unpack_from('<Q', b, aci + 0x10)[0]


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        while chunk := f.read(8 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--game', required=True, choices=sorted(GAMES))
    ap.add_argument('--copy', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    name, tid, version, key = GAMES[a.game]
    copy = a.copy.resolve()
    if not (copy / 'exefs').is_dir() or not (copy / 'romfs').is_dir():
        sys.exit(f'{copy}: needs exefs/ and romfs/')
    found = title_id(copy / 'exefs' / 'main.npdm')
    if found != tid:
        sys.exit(f'{copy}: title id {found}, expected {tid} for {a.game}')
    files = sorted(str(p.relative_to(copy)) for top in ('exefs', 'romfs') for p in (copy / top).rglob('*')
                   if p.is_file())
    with ThreadPoolExecutor(a.jobs) as ex:
        hashes = list(ex.map(lambda r: sha256(copy / r), files))
    out = ROOT / 'games' / a.game / 'manifest.toml'
    out.parent.mkdir(parents=True, exist_ok=True)
    total = sum((copy / r).stat().st_size for r in files)
    lines = [
        f'# Country-Box: manifest of {name} {version} (base game, no update), for the launcher\'s copy check.',
        f'# Hashes and sizes only, measured from a retail dump on {datetime.date.today()} by launcher/make_manifest.py.',
        'format = 1',
        f'game = "{a.game}"',
        f'name = "{name}"',
        f'title_id = "{tid}"',
        f'version = "{version}"',
        f'version_key = "{key}"',
        f'files = {len(files)}',
        f'bytes = {total}',
    ]
    for r, h in zip(files, hashes):
        lines += ['', '[[file]]', f'path = "{r}"', f'size = {(copy / r).stat().st_size}', f'sha256 = "{h}"']
    out.write_text('\n'.join(lines) + '\n')
    print(f'{out}: {len(files)} files, {total:,} bytes')


if __name__ == '__main__':
    main()
