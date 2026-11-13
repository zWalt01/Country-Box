#!/usr/bin/env python3
# Country-Box: assembles the Prepare payload (prep/), everything the launcher needs on the player's machine to
# build a game from their own copy: the Python lifter, the runtime's C/C++ sources, the build tools, the per-game
# manifests and shader lists, and the prebuilt shader translator. NOTHING derived from a game ships in it (the
# generated C and the objects are produced on the player's machine from their copy — nothing from a game is ever distributed).
#
#   python3 launcher/make_prep_payload.py OUT [--xlate DIR]
#
# OUT gets core/{recomp,runtime,tools}, games/<game>, build/xlate/mp4_xlate{,.id}, VERSION and lifter.id (a
# SHA-256 over the lifter's own files: Prepare re-lifts only when it changes; chunks that did not change keep
# their mtime, so ninja does not recompile them either). --xlate takes the translator elsewhere (a build made by
# tools/build_xlate.py); by default core/build/xlate inside this repository.
import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CORE = ROOT / 'core'
TOOLS = ['build_linux.py', 'build_game.py', 'build_shaders.py', 'romfs_index.py', 'check_content.py', 'hostmem.py',
         'requirements.txt']


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{" ".join(str(c) for c in cmd)} failed: {r.stderr.strip()}')
    return r.stdout


def copy_tree(src: Path, dst: Path, skip_dirs=(), skip_files=()):
    # Python's caches never ship: they hold the absolute path of the machine that ran the lifter.
    skip_dirs = set(skip_dirs) | {'__pycache__'}
    for p in src.rglob('*'):
        rel = p.relative_to(src)
        if any(part in skip_dirs for part in rel.parts) or p.suffix == '.pyc':
            continue
        q = dst / rel
        if p.is_dir():
            q.mkdir(parents=True, exist_ok=True)
            continue
        if not p.is_file() or p.name in skip_files:
            continue
        q.parent.mkdir(parents=True, exist_ok=True)
        if not q.is_file() or q.read_bytes() != p.read_bytes():
            shutil.copy2(p, q)


def main():
    ap = argparse.ArgumentParser(description='Assemble the Prepare payload (prep/) next to the launcher.')
    ap.add_argument('out', type=Path)
    ap.add_argument('--xlate', type=Path, default=CORE / 'build' / 'xlate')
    a = ap.parse_args()
    out = a.out.resolve()
    xlate = a.xlate.resolve()
    for f in (xlate / 'mp4_xlate', xlate / 'mp4_xlate.id'):
        if not f.is_file():
            sys.exit(f'{f} is missing (build it with core/tools/build_xlate.py)')
    out.mkdir(parents=True, exist_ok=True)
    # core/recomp (the lifter), core/tools (the build), core/runtime (sources: C, C++, headers, third_party)
    copy_tree(CORE / 'recomp', out / 'core' / 'recomp')
    copy_tree(CORE / 'runtime', out / 'core' / 'runtime')
    (out / 'core' / 'tools').mkdir(parents=True, exist_ok=True)
    for name in TOOLS:
        shutil.copy2(CORE / 'tools' / name, out / 'core' / 'tools' / name)
    # games/<game>: manifests and shader lists (hashes and variant lists, no game data)
    copy_tree(ROOT / 'games', out / 'games')
    # the prebuilt translator (NativeAOT: needs no .NET at the player's machine) and its identity
    (out / 'build' / 'xlate').mkdir(parents=True, exist_ok=True)
    for name in ('mp4_xlate', 'mp4_xlate.id'):
        shutil.copy2(xlate / name, out / 'build' / 'xlate' / name)
        (out / 'build' / 'xlate' / name).chmod(0o755 if name == 'mp4_xlate' else 0o644)
    shutil.copy2(ROOT / 'VERSION', out / 'VERSION')
    # requirements.txt: only what Prepare's Python needs (the lifter), pinned; the hint names the exact commands
    (out / 'requirements.txt').write_text('capstone==5.0.9\nnumpy==2.5.4\nunicorn==2.1.4\n')
    # lifter.id: what Prepare compares to decide whether a lift must run again
    h = hashlib.sha256()
    for p in sorted((out / 'core' / 'recomp').rglob('*.py')):
        h.update(p.name.encode()); h.update(p.read_bytes())
    for name in TOOLS[:-1]:
        h.update(name.encode()); h.update((out / 'core' / 'tools' / name).read_bytes())
    (out / 'lifter.id').write_text(h.hexdigest() + '\n')
    # nothing derived from any game may be in there: the payload is ours to ship
    dirty = [str(p) for p in out.rglob('*') if p.is_file() and p.suffix in ('.nso', '.npdm', '.nsp', '.xci')]
    if dirty:
        sys.exit(f'game files in the payload: {dirty}')
    print(f'payload in {out} (lifter.id {h.hexdigest()[:12]})')


if __name__ == '__main__':
    main()
