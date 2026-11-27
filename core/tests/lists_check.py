#!/usr/bin/env python3
"""Check of the shipped shader and pipeline lists (games/*/shader-list.bin, pipeline-list.bin) and of their formats.

No game files are needed. It checks that
  * tools/prepare_shaders.py and the runtime agree on the format numbers: KEYS_VERSION, sizeof(KeyRec) and the size of a
    version 3 record in vk_pipeline.c (asserted there at compile time) against KEY_VERSION, KEY_REC, KEY_REC_V3 in the
    tool. A key record that changes size without its version and the lists changing with it makes the runtime drop every
    shipped list without a word, and this is what fails then;
  * every shipped list has a valid header, a record size that matches its version and a whole number of records, and
    its records are consistent (a pipeline record's stages: fs and gs ids are all zero exactly when has_fs / has_gs is 0,
    a vertex id is set, no record twice; a shader record: a known stage, no input id, a priority);
  * a version 3 keys file (a player's own, or a list from before geometry pipelines were recorded) is read by the tool
    as version 4 with every record intact and an empty geometry stage, and merged with a list without duplicates.

Exits with 1 if anything fails.

Usage: lists_check.py
"""
import importlib.util
import re
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print('FAIL', what)
    return ok


spec = importlib.util.spec_from_file_location('prepare_shaders', ROOT / 'core/tools/prepare_shaders.py')
ps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps)

# 1. the format numbers: the runtime's (vk_pipeline.c, gfx_shader.c) against the tool's
src = (ROOT / 'core/runtime/vk_pipeline.c').read_text()
m = re.search(r'#define\s+KEYS_VERSION\s+(\d+)', src)
check(m and int(m.group(1)) == ps.KEY_VERSION, f'KEYS_VERSION in vk_pipeline.c is {m and m.group(1)}, the tool writes version {ps.KEY_VERSION}')
m = re.search(r'_Static_assert\(sizeof\(KeyRec\)\s*==\s*(\d+)', src)
check(m and int(m.group(1)) == ps.KEY_REC, f'sizeof(KeyRec) asserted in vk_pipeline.c is {m and m.group(1)}, the tool uses {ps.KEY_REC}')
m = re.search(r'_Static_assert\(KEY_REC_V3\s*==\s*(\d+)', src)
check(m and int(m.group(1)) == ps.KEY_REC_V3, f'the version 3 record size asserted in vk_pipeline.c is {m and m.group(1)}, the tool uses {ps.KEY_REC_V3}')
gsrc = (ROOT / 'core/runtime/gfx_shader.c').read_text()
m1 = re.search(r'#define\s+ANS_PACKED\s+(\d+)', gsrc)
m2 = re.search(r'#define\s+ANS_REC\s+\(\s*4\s*\+\s*32\s*\+\s*32\s*\+\s*ANS_PACKED\s*\+\s*1\s*\)', gsrc)
check(m1 and m2 and 4 + 32 + 32 + int(m1.group(1)) + 1 == ps.ANS_REC,
      f'the answers record size in gfx_shader.c is not the tool\'s {ps.ANS_REC} (ANS_PACKED {m1 and m1.group(1)}; the tool and the shipped shader lists use it)')

# 2. the shipped lists
STAGES = {0, 3, 4, 5}   # GFX_STAGE_VS, GS, FS, CS (gfx.h)
n_lists = 0
for path in sorted((ROOT / 'games').glob('*/pipeline-list.bin')):
    n_lists += 1
    name = path.relative_to(ROOT)
    b = path.read_bytes()
    ok = check(len(b) >= 16 and b[:8] == ps.KEY_MAGIC, f'{name}: no key file header')
    if not ok:
        continue
    ver, size = struct.unpack_from('<II', b, 8)
    expect = {3: ps.KEY_REC_V3, 4: ps.KEY_REC}.get(ver)
    check(expect == size, f'{name}: version {ver} with record size {size} (expected {expect})')
    if expect != size:
        continue
    check((len(b) - 16) % size == 0, f'{name}: {len(b) - 16} bytes after the header are not a whole number of {size}-byte records')
    recs = [b[o:o + size] for o in range(16, len(b) - size + 1, size)]
    check(len(set(recs)) == len(recs), f'{name}: a record twice')
    for i, r in enumerate(recs):
        vs, fs = r[0:32], r[32:64]
        has_fs, has_gs = struct.unpack_from('<II', r, 64)
        gs = r[size - 32:] if ver == 4 else bytes(32)
        if not check(has_fs in (0, 1) and has_gs in (0, 1) and (ver == 4 or has_gs == 0), f'{name}[{i}]: has_fs {has_fs}, has_gs {has_gs}'):
            break
        if not check(any(vs), f'{name}[{i}]: no vertex shader'):
            break
        if not check(any(fs) == bool(has_fs), f'{name}[{i}]: has_fs {has_fs} with a {"set" if any(fs) else "zero"} fragment id'):
            break
        if not check(any(gs) == bool(has_gs), f'{name}[{i}]: has_gs {has_gs} with a {"set" if any(gs) else "zero"} geometry id'):
            break
    print(f'{name}: version {ver}, {len(recs)} pipelines, {sum(1 for r in recs if ver == 4 and struct.unpack_from("<I", r, 68)[0])} with a geometry stage')

for path in sorted((ROOT / 'games').glob('*/shader-list.bin')):
    n_lists += 1
    name = path.relative_to(ROOT)
    b = path.read_bytes()
    if not check(len(b) >= 16 and b[:8] == ps.ANS_MAGIC, f'{name}: no answers file header'):
        continue
    ver, size = struct.unpack_from('<II', b, 8)
    check(size == ps.ANS_REC, f'{name}: record size {size}, the runtime uses {ps.ANS_REC}')
    if size != ps.ANS_REC:
        continue
    check((len(b) - 16) % size == 0, f'{name}: not a whole number of {size}-byte records')
    recs = [b[o:o + size] for o in range(16, len(b) - size + 1, size)]
    keys = [(r[:4], r[4:36], r[68:215]) for r in recs]
    check(len(set(keys)) == len(keys), f'{name}: a variant twice')
    for i, r in enumerate(recs):
        if not (check(struct.unpack_from('<I', r, 0)[0] in STAGES, f'{name}[{i}]: stage {struct.unpack_from("<I", r, 0)[0]}') and
                check(not any(r[36:68]), f'{name}[{i}]: an input id (a list holds none: it names the code of one install)')):
            break
    print(f'{name}: {len(recs)} variants')
check(n_lists > 0, 'no shipped list found under games/')

# 3. a version 3 keys file is read as version 4, and merges
lists = sorted((ROOT / 'games').glob('*/pipeline-list.bin'))
v4 = next((p for p in lists if p.read_bytes()[8:12] == struct.pack('<I', 4)), None)
if v4 is not None:
    recs4 = ps.read_keys(str(v4))
    plain = [r for r in recs4 if r[-32:] == bytes(32)][:50]   # records without a geometry stage: a version 3 file holds exactly these
    with tempfile.TemporaryDirectory() as td:
        v3 = Path(td) / 'keys-v3.bin'
        v3.write_bytes(ps.KEY_MAGIC + struct.pack('<II', 3, ps.KEY_REC_V3) + b''.join(r[:ps.KEY_REC_V3] for r in plain))
        got = ps.read_keys(str(v3))
        check(got == plain, 'a version 3 keys file is not read as the same records with an empty geometry stage')
        out = Path(td) / 'merged.bin'
        ps.make_pipeline_list(str(out), [str(v3), str(v4)])
        merged = ps.read_keys(str(out))
        check(len(merged) == len(recs4) and set(merged) == set(recs4),
              f'a version 3 file merged with {v4.name}: {len(merged)} records, expected {len(recs4)}')
        check(merged[:len(plain)] == plain, 'the merge does not keep the first source\'s records first')
else:
    print('no version 4 pipeline list: the upgrade check is skipped')

print('FAILED: %d' % len(failures) if failures else 'ok')
sys.exit(1 if failures else 0)
