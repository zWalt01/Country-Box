# Adapted from totk-pc-recomp tools/prepare_shaders.py @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026
# Valiantsin Dzerakh): the seed files and the --prepare-shaders run. The list is this project's own (shader answers met in
# play, below) instead of MacNativeGames' TSV, the programs come from tools/shader_inventory.py, and there are no default
# variants.
"""Make the shader variant and pipeline lists a release ships (games/<game>/), from the files of builds that played.

Country-Box: the preparation itself is the runtime's (runtime/shader_prep.c): at the first start, or after the translator
or the GPU driver changed, the game shows a blocking "Preparing shaders" screen, finds every listed program in the
player's own game files (runtime/shader_src_unity.c for Returns HD), translates every variant, builds every listed
pipeline and every stage library into the driver cache; `country-box --prepare-shaders` does the same without a window.
This tool only makes the lists; the seed-file preparation below (upstream's, with tools/shader_inventory.py for a Retro
RomFS) is kept for reference and is not what the runtime runs.

The list names shader variants players met: a code hash (runtime/gfx_shader.c build_code_hash) and the translator
answers it was translated for. It is a file in the layout of shader-answers.bin, which the game writes next to
pipeline-keys.bin as it plays ("answers met in play" in gfx_shader.c), with every input id zeroed: it holds no game
code, so it can ship with a release.

    python3 tools/prepare_shaders.py --make-list ../games/dkcrhd/shader-list.bin <build>/shader-answers.bin [more...]

The pipeline list is the same for pipelines: the records of pipeline-keys.bin (runtime/vk_pipeline.c "pipeline keys":
the render state of each pipeline play built, its shaders named by input id), again no game code. The runtime merges it
into the install's pipeline-keys.bin before preparing:

    python3 tools/prepare_shaders.py --make-pipeline-list ../games/dkcrhd/pipeline-list.bin <build>/pipeline-keys.bin
"""
import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTROL = 0x880
ANS_MAGIC, PRG_MAGIC = b'TOTKSANS', b'TOTKSPRG'
ANS_REC = 4 + 32 + 32 + 147 + 1                            # gfx_shader.c ANS_REC
LIST_PRIORITY = 1                                          # shader-prepared.bin: warm-up (0 title, 2 other)
BUNDLED_LIST = ROOT / 'data' / 'shader-list.bin'           # release folder layout (tools/make_release.py)
BUNDLED_PIPELINES = ROOT / 'data' / 'pipeline-list.bin'
KEY_MAGIC, KEY_VERSION, KEY_REC = b'TOTKPKEY', 3, 424       # vk_pipeline.c KeysHeader, KEYS_VERSION, sizeof(KeyRec)


def read_records(path):
    """The records of an answers file (shader-answers.bin or a list): [bytes of ANS_REC]."""
    b = Path(path).read_bytes()
    if len(b) < 16 or b[:8] != ANS_MAGIC:
        raise SystemExit(f'{path}: not a shader answers file')
    version, size = struct.unpack_from('<II', b, 8)
    if version != 1 or size != ANS_REC:
        raise SystemExit(f'{path}: answers version {version}, record size {size}; this tool reads 1 and {ANS_REC}')
    return [b[o:o + ANS_REC] for o in range(16, len(b) - ANS_REC + 1, ANS_REC)]


def write_records(path, recs):
    with open(path, 'wb') as f:
        f.write(ANS_MAGIC + struct.pack('<II', 1, ANS_REC))
        for r in recs:
            f.write(r)


def make_list(out, sources):
    """The union of the sources' variants, in the order first met, with input ids zeroed and one record per
    (stage, code hash, answers)."""
    seen, recs = set(), []
    for src in sources:
        for r in read_records(src):
            stage, h, packed = r[:4], r[4:36], r[68:68 + 147]
            if (stage, h, packed) in seen:
                continue
            seen.add((stage, h, packed))
            recs.append(stage + h + bytes(32) + packed + bytes([LIST_PRIORITY]))
    write_records(out, recs)
    print(f'shader list: {len(recs)} variants of {len({r[4:36] for r in recs})} programs from {len(sources)} file(s) '
          f'-> {out}')
    return 0


def read_keys(path):
    """The records of a pipeline keys file (pipeline-keys.bin or a pipeline list): [bytes of KEY_REC]."""
    b = Path(path).read_bytes()
    if len(b) < 16 or b[:8] != KEY_MAGIC:
        raise SystemExit(f'{path}: not a pipeline keys file')
    version, size = struct.unpack_from('<II', b, 8)
    if version != KEY_VERSION or size != KEY_REC:
        raise SystemExit(f'{path}: keys version {version}, record size {size}; this tool reads {KEY_VERSION} and {KEY_REC}')
    return [b[o:o + KEY_REC] for o in range(16, len(b) - KEY_REC + 1, KEY_REC)]


def write_keys(path, recs):
    tmp = Path(str(path) + '.tmp')
    with open(tmp, 'wb') as f:
        f.write(KEY_MAGIC + struct.pack('<II', KEY_VERSION, KEY_REC))
        for r in recs:
            f.write(r)
    tmp.replace(path)


def union_keys(sources):
    """The records of the files in order, each distinct record once (the runtime deduplicates by the same bytes)."""
    seen, recs = set(), []
    for src in sources:
        for r in read_keys(src):
            if r not in seen:
                seen.add(r)
                recs.append(r)
    return recs


def make_pipeline_list(out, sources):
    recs = union_keys(sources)
    write_keys(out, recs)
    print(f'pipeline list: {len(recs)} pipelines from {len(sources)} file(s) -> {out}')
    return 0


def merge_pipelines(build, pipelines):
    """The list's records added to the build's pipeline-keys.bin; its own records (an update over a played install)
    stay first. A keys file of another version is replaced, as mp4.exe would rewrite it."""
    keys = build / 'pipeline-keys.bin'
    own = []
    if keys.is_file():
        try:
            own = read_keys(keys)
        except SystemExit as e:
            print(f'{e}; replaced by the list', flush=True)
    listed = read_keys(pipelines)
    have = set(own)
    added = [r for r in dict.fromkeys(listed) if r not in have]
    if added or not keys.is_file():
        write_keys(keys, own + added)
    print(f'{len(listed)} listed pipelines: {len(added)} added to {len(own)} in pipeline-keys.bin', flush=True)


def prepare(a):
    sys.path.insert(0, str(ROOT / 'tools'))
    from shader_inventory import programs   # needs Python 3.14 (compression.zstd)
    t0 = time.monotonic()
    listed = read_records(a.list or BUNDLED_LIST)
    want = {r[4:36] for r in listed}
    progs = {}
    for h, stage, control, code in programs(a.romfs):
        if h in want and h not in progs:
            c1_off, length = struct.unpack_from('<II', control, 0x700)
            progs[h] = struct.pack('<III', stage, length, length - c1_off) + h + control + code[:length] + \
                code[c1_off:length]
    t1 = time.monotonic()
    recs = [r for r in listed if r[4:36] in progs]
    out = a.build.resolve()
    with open(out / 'seed-programs.bin', 'wb') as f:
        f.write(PRG_MAGIC + struct.pack('<II', 1, 0))
        for body in progs.values():
            f.write(body)
    write_records(out / 'seed-answers.bin', recs)
    pipelines = a.pipelines or (BUNDLED_PIPELINES if BUNDLED_PIPELINES.is_file() else None)
    if pipelines:
        merge_pipelines(out, pipelines)
    print(f'{len(listed)} listed variants of {len(want)} programs: {len(recs)} variants of {len(progs)} programs found '
          f'in the game ({len(want) - len(progs)} programs not); inventory {t1 - t0:.1f} s', flush=True)
    if a.no_run:
        return 0
    r = subprocess.run([str(out / 'mp4.exe'), '--prepare-shaders'], cwd=str(out))
    if not a.keep_seed:
        for name in ('seed-programs.bin', 'seed-answers.bin'):
            (out / name).unlink(missing_ok=True)
    print(f'mp4.exe --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s', flush=True)
    return r.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build', type=Path, nargs='?', help='build directory holding mp4.exe (build\\v131072)')
    ap.add_argument('--version', default='v131072', help='build/extracted/<version> (default v131072)')
    ap.add_argument('--romfs', type=Path, help='merged RomFS directory with romfs.bin and index.json '
                                               '(default build/extracted/<version>/romfs)')
    ap.add_argument('--list', type=Path, help=f'the shader list (default {BUNDLED_LIST.relative_to(ROOT)})')
    ap.add_argument('--pipelines', type=Path, help='the pipeline list merged into pipeline-keys.bin (default '
                                                   f'{BUNDLED_PIPELINES.relative_to(ROOT)} when there is one)')
    ap.add_argument('--no-run', action='store_true', help='write the seed files only')
    ap.add_argument('--keep-seed', action='store_true', help='keep the seed files after mp4.exe ran')
    ap.add_argument('--make-list', type=Path, metavar='OUT', help='write a list from the answers files given after it')
    ap.add_argument('--make-pipeline-list', type=Path, metavar='OUT',
                    help='write a pipeline list from the pipeline-keys.bin files given after it')
    ap.add_argument('sources', type=Path, nargs='*', help='with --make-list: shader-answers.bin files; with '
                                                          '--make-pipeline-list: pipeline-keys.bin files')
    a = ap.parse_args()
    if a.make_pipeline_list:
        sources = ([a.build] if a.build else []) + a.sources
        if not sources:
            ap.error('--make-pipeline-list needs at least one pipeline-keys.bin')
        return make_pipeline_list(a.make_pipeline_list, sources)
    if a.make_list:
        sources = ([a.build] if a.build else []) + a.sources
        if not sources:
            ap.error('--make-list needs at least one shader-answers.bin')
        return make_list(a.make_list, sources)
    if not a.build:
        ap.error('the build directory is required')
    a.romfs = a.romfs or ROOT / 'build' / 'extracted' / a.version / 'romfs'
    return prepare(a)


if __name__ == '__main__':
    sys.exit(main())
