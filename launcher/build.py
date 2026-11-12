#!/usr/bin/env python3
# Country-Box: builds the launcher, country-box-launcher (launcher/src, SDL 3 + SDL_Renderer + Dear ImGui, the same
# Dear ImGui as the game runtime: core/runtime/third_party/imgui). Nothing from a game goes in, so the output may live
# in the repository's ignored build/ folder.
#
#   python3 launcher/build.py [--out DIR] [--jobs N]
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
IMGUI = ROOT / 'core' / 'runtime' / 'third_party' / 'imgui'
BACKENDS = HERE / 'third_party' / 'imgui_backends'
WARN = ['-Wall', '-Wno-c23-extensions']
# -ffile-prefix-map: the binary's debug info and assert strings name sources relative to the repository, never the
# builder's home (a release must not carry the maintainer's paths).
COMMON = ['-O2', '-g', '-fno-strict-aliasing', '-D_GNU_SOURCE', f'-ffile-prefix-map={HERE.parent}=.']


def pkg(flag):
    r = subprocess.run(['pkg-config', flag, 'sdl3'], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'pkg-config {flag} sdl3: {r.stderr.strip()}')
    return r.stdout.split()


def main():
    ap = argparse.ArgumentParser(description='Build country-box-launcher.')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'launcher')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    for tool in ('clang', 'clang++', 'ninja', 'pkg-config'):
        if not shutil.which(tool):
            sys.exit(f'missing: {tool}')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    version = (ROOT / 'VERSION').read_text().strip()
    inc = ['-I', str(HERE / 'src'), '-I', str(IMGUI), '-I', str(BACKENDS)] + pkg('--cflags')
    defs = [f'-DCB_VERSION=\\"{version}\\"']
    srcs = sorted(HERE.glob('src/*.c')) + sorted(HERE.glob('src/*.cpp')) + sorted(BACKENDS.glob('*.cpp')) + \
        [IMGUI / n for n in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')]
    q = lambda p: str(p).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')
    lines = [
        '# Written by launcher/build.py; do not edit.',
        f'cflags = {" ".join(["-std=gnu17"] + COMMON + WARN + inc + defs)}',
        f'cxxflags = {" ".join(["-std=c++20"] + COMMON + WARN + inc + defs)}',
        f'libs = {" ".join(pkg("--libs") + ["-ldl", "-lpthread"])}',
        'rule cc', '  command = clang $cflags -MD -MF $out.d -c $in -o $out', '  deps = gcc', '  depfile = $out.d',
        '  description = CC $in',
        'rule cxx', '  command = clang++ $cxxflags -MD -MF $out.d -c $in -o $out', '  deps = gcc', '  depfile = $out.d',
        '  description = CXX $in',
        'rule link', '  command = clang++ -fuse-ld=lld $in $libs -o $out', '  description = LINK $out',
    ]
    objs = []
    for s in srcs:
        o = out / 'obj' / (s.parent.name + '_' + s.stem + '.o')
        lines.append(f'build {q(o)}: {"cc" if s.suffix == ".c" else "cxx"} {q(s)}')
        objs.append(o)
    exe = out / 'country-box-launcher'
    lines += [f'build {q(exe)}: link ' + ' '.join(q(o) for o in objs), f'default {q(exe)}', '']
    text = '\n'.join(lines)
    nf = out / 'build.ninja'
    if not nf.is_file() or nf.read_text() != text:
        nf.write_text(text)
    r = subprocess.run(['nice', '-n', '19', 'ninja', '-C', str(out), '-j', str(a.jobs)])
    if r.returncode:
        sys.exit(r.returncode)
    print(f'built {exe}')


if __name__ == '__main__':
    main()
