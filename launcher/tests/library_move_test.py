#!/usr/bin/env python3
# Country-Box: --move-library on a scratch library (no game needed), under the setups players have: XDG_DATA_HOME set
# or not, a home and folder names with spaces, quotes, accents and $, the same drive (rename) and another drive
# (copy), a folder that already has files, and refusals (inside the library, not empty, a noexec mount).
#
#   python3 launcher/tests/library_move_test.py LAUNCHER [OTHER_DRIVE_DIR]
#
# OTHER_DRIVE_DIR: a writable folder on a different filesystem than /tmp (the cross-drive copy); skipped without it.
# The noexec case runs inside an unprivileged user + mount namespace (unshare -rm); skipped if that is unavailable.
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ODD = "the maintainer's Games (ñ) $HOME"
failures = 0


def check(cond, what):
    global failures
    print(('ok    ' if cond else 'FAIL  ') + what)
    failures += not cond


def run(exe, env, *args):
    p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=600)
    return p.returncode, p.stdout, p.stderr


def fake_library(home):
    """A library as Prepare leaves it: a build with its binary, saves and settings, the C made from the game, logs,
    library.ini (one build= inside the library), and the tools that stay (python/)."""
    b = home / 'games' / 'dkctf'
    (b / 'profile' / 'save').mkdir(parents=True)
    (b / 'profile' / 'save' / 'slot0.bin').write_bytes(b'save data')
    (b / 'mp4.ini').write_text('[video]\nwidth = 1280\n')
    exe = b / 'country-box'
    exe.write_bytes(b'\x7fELF' + b'\0' * 4096)
    exe.chmod(0o755)
    (home / 'gen' / 'dkctf').mkdir(parents=True)
    (home / 'gen' / 'dkctf' / 'chunk_000.c').write_text('int x;\n')
    (home / 'logs').mkdir()
    (home / 'logs' / 'prepare.log').write_text('log\n')
    (home / 'python' / 'bin').mkdir(parents=True)
    (home / 'library.ini').write_text(f'[dkcrhd]\nbuild = {home}/games/dkcrhd\n')


def moved_ok(src, dst, what):
    b = dst / 'games' / 'dkctf'
    check((b / 'profile' / 'save' / 'slot0.bin').read_bytes() == b'save data', f'{what}: saves moved')
    check(os.access(b / 'country-box', os.X_OK), f'{what}: the game binary is still runnable')
    check((dst / 'gen' / 'dkctf' / 'chunk_000.c').is_file() and (dst / 'logs' / 'prepare.log').is_file(), f'{what}: C and logs moved')
    check(f'build = {dst}/games/dkcrhd' in (dst / 'library.ini').read_text(), f'{what}: library.ini build= follows the move')
    check(not (src / 'games').exists() and not (src / 'gen').exists(), f'{what}: nothing left behind')


def main():
    exe = str(Path(sys.argv[1]).resolve())
    other = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None
    tmp = Path(tempfile.mkdtemp(prefix='cb-move-test-'))
    try:
        for xdg in (False, True):
            label = 'XDG_DATA_HOME set' if xdg else 'XDG_DATA_HOME unset'
            case = tmp / ('xdg' if xdg else 'plain')
            home_dir = case / f'home of {ODD}'
            env = {k: v for k, v in os.environ.items() if k not in ('XDG_DATA_HOME', 'CB_LAUNCHER_HOME')}
            env['HOME'] = str(home_dir)
            if xdg:
                env['XDG_DATA_HOME'] = str(case / 'data dir')
                default = case / 'data dir' / 'country-box'
            else:
                default = home_dir / '.local' / 'share' / 'country-box'
            fake_library(default)
            # same drive, into a folder with odd characters that does not exist yet
            target = case / ODD / 'Library'
            rc, out, err = run(exe, env, '--move-library', str(target))
            ans = json.loads(out or '{}')
            check(rc == 0 and ans.get('ok') and ans.get('to') == str(target), f'{label}: same drive move ({err.strip() or "ok"})')
            moved_ok(default, target, f'{label}, same drive')
            check((default / 'python' / 'bin').is_dir(), f'{label}: python/ stays in the default place')
            check((default / 'library-location').read_text().strip() == str(target), f'{label}: the new place is recorded')
            rc, out, _ = run(exe, env, '--status')
            check(json.loads(out)['home'] == str(target), f'{label}: later runs use the moved library')
            # a folder that already has files: a Country-Box folder inside it
            busy = case / 'busy folder'
            busy.mkdir()
            (busy / 'other.txt').write_text('x')
            rc, out, _ = run(exe, env, '--move-library', str(busy))
            check(rc == 0 and json.loads(out)['to'] == str(busy / 'Country-Box'), f'{label}: a folder with files gets Country-Box/ inside')
            moved_ok(target, busy / 'Country-Box', f'{label}, into a folder with files')
            cur = busy / 'Country-Box'
            # refusals
            rc, out, _ = run(exe, env, '--move-library', str(cur / 'games' / 'x'))
            check(rc == 1 and 'inside your library' in json.loads(out)['headline'], f'{label}: refuses a folder inside the library')
            full = case / 'full'
            (full / 'Country-Box').mkdir(parents=True)
            (full / 'Country-Box' / 'f').write_text('x')
            rc, out, _ = run(exe, env, '--move-library', str(full))   # full/Country-Box exists and has files
            check(rc == 1 and 'not empty' in json.loads(out)['headline'], f'{label}: refuses a folder that is not empty')
            check((cur / 'games' / 'dkctf' / 'country-box').is_file(), f'{label}: a refused move leaves the library as it was')
            # another drive (copy), then back to the default place (the record goes away)
            if other:
                far = other / f'cb-move-test {ODD}' / ('xdg' if xdg else 'plain')
                shutil.rmtree(far, ignore_errors=True)
                rc, out, err = run(exe, env, '--move-library', str(far))
                check(rc == 0 and json.loads(out)['ok'], f'{label}: move to another drive ({err.strip().splitlines()[-1] if err.strip() else "ok"})')
                moved_ok(cur, far, f'{label}, another drive')
                check(not cur.exists(), f'{label}: the emptied old folder is removed')
                cur = far
            rc, out, _ = run(exe, env, '--move-library', str(default))
            check(rc == 0 and json.loads(out)['ok'], f'{label}: move back to the default place')
            moved_ok(cur, default, f'{label}, back home')
            check(not (default / 'library-location').exists(), f'{label}: back home, nothing is recorded')
            if other:
                shutil.rmtree(other / f'cb-move-test {ODD}', ignore_errors=True)
            # --home / CB_LAUNCHER_HOME: the folder is fixed for the run
            rc, _, err = run(exe, dict(env, CB_LAUNCHER_HOME=str(default)), '--move-library', str(case / 'x'))
            check(rc == 1 and 'CB_LAUNCHER_HOME' in err, f'{label}: CB_LAUNCHER_HOME fixes the folder')
        # noexec: a tmpfs mounted noexec in a private namespace
        if shutil.which('unshare'):
            case = tmp / 'noexec'
            home_dir = case / 'home'
            default = home_dir / '.local' / 'share' / 'country-box'
            fake_library(default)
            mnt = case / 'noexec drive'
            mnt.mkdir()
            env = {k: v for k, v in os.environ.items() if k not in ('XDG_DATA_HOME', 'CB_LAUNCHER_HOME')}
            env['HOME'] = str(home_dir)
            script = f'mount -t tmpfs -o noexec tmpfs "$1" && exec "$2" --move-library "$1/lib"'
            p = subprocess.run(['unshare', '-rm', 'sh', '-c', script, 'sh', str(mnt), exe], env=env, capture_output=True, text=True)
            if 'mount' in p.stderr and not p.stdout:
                print('skip  noexec (no user namespaces here):', p.stderr.strip())
            else:
                ans = json.loads(p.stdout)
                check(p.returncode == 1 and 'run from that drive' in ans['headline'] and 'noexec' in ans['explain'],
                      'refuses a noexec drive, saying why')
                check((default / 'games' / 'dkctf' / 'country-box').is_file(), 'a noexec refusal leaves the library as it was')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print('library move test:', 'passed' if not failures else f'{failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
