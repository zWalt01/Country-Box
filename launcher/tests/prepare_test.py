#!/usr/bin/env python3
# Country-Box: test of Prepare and Import (country-box-launcher --prepare / --import-build). The fast paths run
# without a toolchain: the plain-words errors for missing tools and no disk space, and every import check (a
# build folder made elsewhere, wrong game, wrong copy, an archive, and the player's own saves staying put).
# The real pipeline (lift, compile, shader preparation, resume) is exercised end to end with a full copy, not here.
#
#   python3 launcher/tests/prepare_test.py build/launcher/country-box-launcher RETURNS_COPY
import json
import os
import shutil
import subprocess
import sys
import tempfile
import tarfile
from pathlib import Path

fails = 0


def check(ok, what):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + what)
    fails += not ok


def run(exe, home, *args, **env):
    e = dict(os.environ, CB_LAUNCHER_HOME=str(home))
    e.update({k: str(v) for k, v in env.items()})
    p = subprocess.run([str(exe), *args], env=e, capture_output=True, text=True, timeout=600)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, None


def make_build(root, game, manifest, version='0.1.0'):
    b = root / 'build'
    b.mkdir(parents=True, exist_ok=True)
    exe = b / 'country-box'
    exe.write_text('#!/bin/sh\necho stub-country-box\n')
    exe.chmod(0o755)
    (b / 'mp4_xlate.id').write_text('test-translator-id\n')
    (b / 'shader-list.bin').write_bytes(b'\x00' * 8)
    (b / 'profile').mkdir()
    (b / 'profile' / 'save.dat').write_text('the player would lose this')
    (b / 'mp4.ini').write_text('[video]\n')
    (b / 'shader-prepared.id').write_text('other-machine marker\n')
    stamp = {'country-box': version, 'game': game, 'copy_manifest': manifest, 'toolchain': 'clang test',
             'gen_opt': '-O1', 'date': '2026-11-12'}
    (b / 'build-stamp.json').write_text(json.dumps(stamp))
    return b


def main():
    exe, rcopy = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix='cb-prepare-test-'))
    home = tmp / 'home'
    try:
        code, _ = run(exe, home, '--add', 'dkcrhd', str(rcopy), '--quick')
        check(code == 0, 'the retail copy is added')
        manifest = (home / 'library.ini').read_text().split('copy_manifest = ')[1].split('\n')[0].strip()

        # --- Prepare's plain-words errors
        code, r = run(exe, home, '--prepare', 'dkcrhd', PATH=str(tmp / 'no-tools'))
        check(code == 1 and r and r['kind'] == 'missing_tools' and 'clang' in r['explain'], 'missing tools are named, '
              'with the import hint')
        check(r and 'Import a build' in r['hint'], 'the missing-tools hint offers importing a build')
        code, r = run(exe, home, '--prepare', 'dkcrhd', CB_TEST_TOOLS_OK=1, CB_TEST_DISK_FREE_GB='0.1')
        check(code == 1 and r and r['kind'] == 'disk' and 'GB' in r['explain'], 'not enough space says how much it '
              'needs and how much there is')
        code, r = run(exe, home, '--prepare', 'dkcrhd', CB_PREP_SOURCES=str(tmp / 'no-payload'), CB_TEST_TOOLS_OK=1)
        check(code == 1 and r and r['kind'] == 'payload', 'a missing build kit is named')
        code, r = run(exe, home, '--prepare', 'dkctf')
        check(code == 1 and r is None, 'preparing without a copy is refused before anything runs')

        # --- Import: the checks
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(tmp / 'nowhere'))
        check(code == 1 and r and r['kind'] == 'not_found', 'a missing build folder is named')
        empty = tmp / 'not-a-build'
        empty.mkdir()
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(empty))
        check(code == 1 and r and r['kind'] == 'not_build' and 'build-stamp.json' in r['explain'],
              'a folder without a stamp is refused with the reason')
        wrong_game = make_build(tmp / 'wrong-game', 'dkctf', manifest)
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(wrong_game))
        check(code == 1 and r and r['kind'] == 'wrong_game', 'a build of the other game is refused')
        wrong_copy = make_build(tmp / 'wrong-copy', 'dkcrhd', 'f' * 64)
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(wrong_copy))
        check(code == 1 and r and r['kind'] == 'wrong_copy' and 'another version' in r['headline'],
              'a build from another copy is refused')

        # --- Import: the install
        b = make_build(tmp / 'the-build', 'dkcrhd', manifest)
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(b))
        dst = home / 'games' / 'dkcrhd'
        check(code == 0 and r and r['ok'] and r['build'] == str(dst), 'a matching build installs')
        check((dst / 'country-box').is_file() and os.access(dst / 'country-box', os.X_OK), 'the binary lands, '
              'executable')
        check(not (dst / 'profile').exists() and not (dst / 'mp4.ini').exists() and not (dst / 'shader-prepared.id').exists(),
              "the other machine's saves, settings and shader marker never travel")
        # a player's own things survive a re-import
        (dst / 'profile').mkdir()
        (dst / 'profile' / 'save.dat').write_text('my save')
        (dst / 'mp4.ini').write_text('[video]\nvsync=mailbox\n')
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(b))
        check(code == 0 and (dst / 'profile' / 'save.dat').read_text() == 'my save' and 'vsync' in
              (dst / 'mp4.ini').read_text(), 'importing again keeps this computer\'s saves and settings')

        # --- Import: an archive
        arch = tmp / 'build.tar'
        with tarfile.open(arch, 'w') as t:
            t.add(b, arcname='build')
        code, r = run(exe, home, '--import-build', 'dkcrhd', str(arch))
        check(code == 0 and r and r['ok'], 'a .tar of a build installs (top folder stepped into)')

        # --- the library after an import
        code, r = run(exe, home, '--status')
        game = [g for g in r['games'] if g['id'] == 'dkcrhd'][0]
        check(game['build'] == str(dst), 'the library points at the imported build')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('prepare_test:', 'PASS' if fails == 0 else f'{fails} FAILURES')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
