#!/usr/bin/env python3
# Country-Box: test of the launcher's copy check and library (country-box-launcher --verify / --add / --set-build /
# --status). Builds broken copies out of a real one in a scratch folder (symbolic links to the player's files, plus
# private altered copies of two small files), so the real copy is only read. Uses a scratch library.
#
#   python3 launcher/tests/launcher_cli_test.py build/launcher/country-box-launcher RETURNS_COPY [TF_COPY] [RETURNS_BUILD]
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

fails = 0


def check(ok, what):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + what)
    fails += not ok


def run(exe, home, *args):
    env = dict(os.environ, CB_LAUNCHER_HOME=str(home), CB_SETTINGS_FILE=str(home / 'settings.ini'))
    p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=600)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, None


def linked_copy(src, dst):
    for top in ('exefs', 'romfs'):
        for p in (src / top).rglob('*'):
            q = dst / p.relative_to(src)
            if p.is_dir():
                q.mkdir(parents=True, exist_ok=True)
            else:
                q.parent.mkdir(parents=True, exist_ok=True)
                q.symlink_to(p)


def altered(path):
    """Replaces a link with a private copy that differs in one byte (same size)."""
    real = path.resolve()
    path.unlink()
    shutil.copyfile(real, path)
    with open(path, 'r+b') as f:
        f.seek(min(10, real.stat().st_size - 1))
        b = f.read(1)
        f.seek(-1, 1)
        f.write(bytes([b[0] ^ 0xFF]))


def main():
    exe, rcopy = sys.argv[1], Path(sys.argv[2]).resolve()
    tfcopy = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
    rbuild = Path(sys.argv[4]).resolve() if len(sys.argv) > 4 else None
    tmp = Path(tempfile.mkdtemp(prefix='cb-launcher-test-'))
    home = tmp / 'home'
    try:
        code, r = run(exe, home, '--verify', 'dkcrhd', str(rcopy))
        check(code == 0 and r and r['result'] == 'ok', 'the retail copy passes')
        code, r = run(exe, home, '--verify', 'dkcrhd', str(rcopy / 'exefs'), '--quick')
        check(r and r['result'] == 'ok' and r['root'] == str(rcopy) and r['note'], 'choosing exefs/ checks the folder above')
        code, r = run(exe, home, '--verify', 'dkcrhd', str(tmp / 'nowhere'))
        check(code == 1 and r and r['result'] == 'not_found', 'a missing folder is named')
        (tmp / 'packed').mkdir()
        (tmp / 'packed' / 'game.nsp').write_bytes(b'')
        code, r = run(exe, home, '--verify', 'dkcrhd', str(tmp / 'packed'))
        check(r and r['result'] == 'packed' and 'decrypt' in r['explain'], 'a packed file is refused without unpacking')
        (tmp / 'half' / 'exefs').mkdir(parents=True)
        code, r = run(exe, home, '--verify', 'dkcrhd', str(tmp / 'half'))
        check(r and r['result'] == 'not_extracted' and any('romfs' in d for d in r['details']), 'a copy without romfs/ says so')

        miss = tmp / 'missing'
        linked_copy(rcopy, miss)
        gone = sorted(p for p in (miss / 'romfs').rglob('*') if p.is_symlink())[:3]
        for p in gone:
            p.unlink()
        code, r = run(exe, home, '--verify', 'dkcrhd', str(miss))
        check(r and r['result'] == 'missing_files' and r['missing'] == 3, 'three missing files are counted')
        check(r and all(str(p.relative_to(miss)) in r['details'] for p in gone), 'and named')

        dmg = tmp / 'damaged'
        linked_copy(rcopy, dmg)
        small = min((p for p in (dmg / 'romfs').rglob('*') if p.is_symlink()), key=lambda p: p.resolve().stat().st_size)
        altered(small)
        code, r = run(exe, home, '--verify', 'dkcrhd', str(dmg))
        check(r and r['result'] == 'damaged_files' and r['damaged'] == 1 and str(small.relative_to(dmg)) in r['details'],
              'a file with one byte changed is found by its hash')

        upd = tmp / 'update'
        linked_copy(rcopy, upd)
        altered(upd / 'exefs' / 'main')
        code, r = run(exe, home, '--verify', 'dkcrhd', str(upd), '--quick')
        check(r and r['result'] == 'wrong_version' and 'update' in r['explain'], 'other program files read as another version')

        if tfcopy:
            code, r = run(exe, home, '--verify', 'dkcrhd', str(tfcopy), '--quick')
            check(r and r['result'] == 'other_game' and r['other_game'] == 'dkctf', 'the other game is recognised')
            code, r = run(exe, home, '--verify', 'dkctf', str(tfcopy))
            check(code == 0 and r and r['result'] == 'ok', 'the Tropical Freeze copy passes')

        code, r = run(exe, home, '--add', 'dkcrhd', str(rcopy))
        code, st = run(exe, home, '--status')
        g = {x['id']: x for x in st['games']} if st else {}
        check(g.get('dkcrhd', {}).get('state') == 'needs_build' and g['dkcrhd']['copy'] == str(rcopy),
              'an added copy without a build needs preparing')
        check(g.get('dkctf', {}).get('state') == 'not_installed', 'the other game stays not installed')
        if rbuild:
            code, st = run(exe, home, '--set-build', 'dkcrhd', str(rbuild))
            g = {x['id']: x for x in st['games']} if st else {}
            check(code == 0 and g['dkcrhd']['state'] in ('ready', 'needs_shaders', 'update_available') and
                  g['dkcrhd']['build_version'], 'with a build it is playable and its version is read (--settings-dump)')
        lib = (home / 'library.ini').read_text()
        check('copy = ' + str(rcopy) in lib, 'library.ini records the copy')
    finally:
        shutil.rmtree(tmp)
    print('launcher cli test:', 'passed' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
