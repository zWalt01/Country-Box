#!/usr/bin/env python3
# Country-Box: the Linux build of a game (replaces tools/build_game.py, whose toolchain, job objects and PE checks are
# Windows-only; its layout, chunk list and module glue are reused from it).
"""Compile the generated C of one game and link it with the runtime into <out>/country-box (Linux, x86-64-v3).

    python3 tools/build_linux.py --gen GEN --exefs EXEFS --romfs ROMFS --out OUT [--version v0] [--jobs N]

GEN is the generator's output (layout.json, layout.h, patches.h, <module>/manifest.json and chunks), EXEFS and ROMFS the
player's extracted game (they become the binary's defaults; --exefs/--romfs still override at run time). OUT holds
everything derived from the game (objects, modules.c, romfs index, the binary): it must be outside the repository.

Lifted code is built as LAS Recomp builds it plus -mno-red-zone (runtime/kernel.c kern_ap_stub writes a return slot
below an interrupted rsp) and linked as one contiguous run after the runtime's objects, with runtime/kernel_probe_spin.c
inside it (the preemption test's game code, kernel_probe.c); kernel.c ap_init checks that no host function falls in
that run. Ninja rebuilds only what changed.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_game as bg   # noqa: E402  (layout, chunks and module glue; nothing Windows-only runs on import)

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3', '-mno-red-zone']
RT_FLAGS = ['-std=gnu17', '-O2', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing', '-march=x86-64-v3']
CXX_FLAGS = ['-std=c++20', '-O2', '-g', '-Wall', '-fno-strict-aliasing', '-march=x86-64-v3']
PKGS = ['sdl3', 'vulkan', 'opus', 'vpx', 'zlib']   # zlib: shader_src_dktf.c (Tropical Freeze's shader pools)
LINK_LIBS = ['-lm', '-lpthread', '-ldl', '-lstdc++']
# Peak memory of one generated-C compile at -O1 (build_game.py: 120-180 MiB per chunk, 568 MiB for main_table.c).
JOB_MEM = 600 << 20


def pkg(flag, names):
    r = subprocess.run(['pkg-config', flag, *names], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'pkg-config {flag} {" ".join(names)}: {r.stderr.strip()}')
    return r.stdout.split()


def runtime_sources():
    """Every runtime C and C++ file of the binary: runtime/*.c, *.cpp, platform/*.c, ui/*.cpp and Dear ImGui (with its
    Vulkan back end). kernel_probe_spin.c is lifted-run code, built with the generated flags."""
    pats = ('*.c', '*.cpp', 'platform/*.c', 'ui/*.cpp', 'third_party/imgui/*.cpp', 'third_party/imgui/backends/*.cpp')
    return sorted(p for pat in pats for p in RUNTIME.glob(pat) if p.name != 'kernel_probe_spin.c')


def exefs_hashes(exefs, names):
    out = {}
    for n in names + ['main.npdm']:
        p = exefs / n
        if not p.is_file():
            sys.exit(f'{p} is missing')
        out[n] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def mem_total():
    with open('/proc/meminfo') as f:
        for line in f:
            if line.startswith('MemTotal:'):
                return int(line.split()[1]) * 1024
    return 8 << 30


def ninja_text(gen, out, mods, gen_opt='-O1'):
    q = lambda p: str(p).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')
    inc = ['-I', str(RUNTIME), '-I', str(gen)]
    rt_inc = inc + ['-I', str(out), '-I', str(RUNTIME / 'third_party'), '-I', str(RUNTIME / 'third_party' / 'vma'),
                    '-I', str(RUNTIME / 'third_party' / 'imgui')] + pkg('--cflags', PKGS)
    lines = [
        '# Written by tools/build_linux.py; do not edit.',
        'ninja_required_version = 1.10',
        f'genflags = {" ".join([gen_opt if f == "-O1" else f for f in GEN_FLAGS] + inc)}',
        f'rtflags = {" ".join(RT_FLAGS + rt_inc)}',
        f'cxxflags = {" ".join(CXX_FLAGS + rt_inc)}',
        f'libs = {" ".join(pkg("--libs", PKGS) + LINK_LIBS)}',
        '',
        'rule cc_gen',
        '  command = clang $genflags -MD -MF $out.d -c $in -o $out',
        '  deps = gcc',
        '  depfile = $out.d',
        '  description = CC $in',
        'rule cc_rt',
        '  command = clang $rtflags -MD -MF $out.d -c $in -o $out',
        '  deps = gcc',
        '  depfile = $out.d',
        '  description = CC $in',
        'rule cxx_rt',
        '  command = clang++ $cxxflags -MD -MF $out.d -c $in -o $out',
        '  deps = gcc',
        '  depfile = $out.d',
        '  description = CXX $in',
        'rule link',
        '  command = clang++ -fuse-ld=lld -Wl,-Map=$out.map @$out.rsp $libs -o $out',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in_newline',
        '  description = LINK $out',
        '',
    ]
    rt, lifted = [], []
    for src in runtime_sources() + [out / 'modules.c']:
        rel = src.relative_to(RUNTIME) if src.is_relative_to(RUNTIME) else Path(src.name)
        o = out / 'obj' / 'runtime' / rel.with_suffix('.o')
        lines.append(f'build {q(o)}: {"cxx_rt" if src.suffix == ".cpp" else "cc_rt"} {q(src)}')
        rt.append(o)
    for mod, files in mods.items():
        for f in files:
            o = out / 'obj' / mod / (f.stem + '.o')
            lines.append(f'build {q(o)}: cc_gen {q(f)}')
            lifted.append(o)
    spin = out / 'obj' / 'lifted' / 'kernel_probe_spin.o'
    lines.append(f'build {q(spin)}: cc_gen {q(RUNTIME / "kernel_probe_spin.c")}')
    run = lifted[:1] + [spin] + lifted[1:]   # inside the lifted run, not at either end
    exe = out / 'country-box'
    lines += ['', f'build {q(exe)}: link ' + ' '.join(q(o) for o in rt + run), f'default {q(exe)}', '']
    return '\n'.join(lines), exe


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--gen', type=Path, required=True)
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--profile', type=Path, help='default profile (saves) directory (default <out>/profile)')
    ap.add_argument('--version', default='v0', help=f'game version key (one of {sorted(bg.DISPLAY_VERSIONS)})')
    ap.add_argument('--jobs', type=int)
    ap.add_argument('--gen-opt', default='-O1', choices=('-O1', '-O2', '-O3', '-Os'),
                    help='optimization level of the lifted code (default -O1; the runtime is always -O2)')
    ap.add_argument('--fonts', type=Path, help='the open fonts (tools/fetch_fonts.py output) to place in <out>/fonts '
                                               '(default $CB_FONTS_CACHE or ~/.cache/country-box/fonts; optional)')
    ap.add_argument('--game', help='games/<game> whose shader lists go next to the binary (default: dkcrhd for a '
                                   'Unity RomFS, dktf for a RomFS with MaterialArchive.arc)')
    a = ap.parse_args()
    gen, out = a.gen.resolve(), a.out.resolve()
    if out.is_relative_to(ROOT.parent):   # ROOT.parent: the Country-Box repository
        sys.exit(f'{out} is inside the repository: game-derived objects must live outside it')
    for tool in ('clang', 'clang++', 'ld.lld', 'ninja', 'pkg-config'):
        if not shutil.which(tool):
            sys.exit(f'missing: {tool}')
    layout = bg.read_layout(gen)
    names = [n for n, _, _ in layout]
    mods = {m: bg.chunk_files(gen, m) for m in names}
    paths = {'exefs': a.exefs.resolve(), 'romfs': a.romfs.resolve(), 'profile': (a.profile or out / 'profile').resolve()}
    out.mkdir(parents=True, exist_ok=True)
    bg.write_if_changed(out / 'modules.c', bg.module_glue(layout, names, exefs_hashes(paths['exefs'], names), paths,
                                                          a.version))
    if (paths['romfs'] / 'index.json').is_file():
        count, size, dt = bg.romfs_index.write(paths['romfs'], out)
    elif paths['romfs'].is_dir():   # the extracted tree: runtime/romfs.c opens each file (ROMFS_DIRECTORY)
        count, size, dt = bg.romfs_index.write_dir(paths['romfs'], out)
    else:
        sys.exit(f'{paths["romfs"]}: no RomFS (neither index.json + romfs.bin nor an extracted directory)')
    print(f'romfs index: {count} files, {size:,} bytes, {dt:.1f} s')
    text, exe = ninja_text(gen, out, mods, a.gen_opt)
    bg.write_if_changed(out / 'build.ninja', text)
    jobs = a.jobs or max(1, min(os.cpu_count() or 1, int(mem_total() * 0.6) // JOB_MEM))
    r = subprocess.run(['ninja', '-C', str(out), '-j', str(jobs)])
    if r.returncode:
        sys.exit(r.returncode)
    # The shader translator next to the binary (runtime/gfx_shader.c looks there first): tools/build_xlate.py's output.
    xlate = ROOT / 'build' / 'xlate' / 'mp4_xlate'
    if xlate.is_file():
        for f in (xlate, xlate.with_name('mp4_xlate.id')):
            if f.is_file() and (not (out / f.name).exists() or (out / f.name).read_bytes() != f.read_bytes()):
                shutil.copy2(f, out / f.name)
    else:
        print(f'warning: {xlate} missing (tools/build_xlate.py): shaders will not translate', file=sys.stderr)
    # Country-Box: the game's shader and pipeline lists (games/<game>/, no game code) next to the binary, where the
    # shader preparation (runtime/shader_prep.c) reads them.
    game = a.game or ('dkcrhd' if (paths['romfs'] / 'Data' / 'globalgamemanagers').is_file()
                      else 'dktf' if (paths['romfs'] / 'MaterialArchive.arc').is_file() else None)
    for name in ('shader-list.bin', 'pipeline-list.bin', 'shader-patches.bin'):
        f = ROOT.parent / 'games' / (game or '-') / name
        if game and f.is_file() and (not (out / name).exists() or (out / name).read_bytes() != f.read_bytes()):
            shutil.copy2(f, out / name)
    # The open system fonts and their licences (not in the repository: tools/fetch_fonts.py) next to the binary, in
    # fonts/, where runtime/ov_system.c looks. Optional: without them a game that asks for a system font gets none.
    src = a.fonts or Path(os.environ.get('CB_FONTS_CACHE') or Path.home() / '.cache' / 'country-box' / 'fonts')
    files = sorted(list(src.glob('*.ttf')) + list(src.glob('OFL-*.txt'))) if src.is_dir() else []
    if files:
        (out / 'fonts').mkdir(exist_ok=True)
        for f in files:
            dst = out / 'fonts' / f.name
            if not dst.exists() or dst.stat().st_size != f.stat().st_size or dst.read_bytes() != f.read_bytes():
                shutil.copy2(f, dst)
        print(f'fonts: {len(files)} files from {src} into {out / "fonts"}')
    else:
        print(f'note: no fonts in {src}: run tools/fetch_fonts.py to ship the open system fonts (optional)')
    print(f'built {exe} ({exe.stat().st_size / 1e6:.0f} MB)')


if __name__ == '__main__':
    main()
