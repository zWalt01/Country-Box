#!/usr/bin/env python3
# Country-Box under RecompLauncher (relocate.h): the data folder is RECOMPLAUNCHER_DATA ({data}), and the first start
# copies in the old Country-Box-data that 1.0.7 to 1.0.9 kept beside the game's folder. Run the way RecompLauncher
# runs it: the extracted AppImage is the game's folder ({install}), started as
# `env -u APPIMAGE RECOMPLAUNCHER=1 RECOMPLAUNCHER_DATA={install}/recompdata {install}/AppRun`, on scratch HOMEs —
# never the real one. No game needed: the old data is a stand-in with saves, settings, a build, logs and caches.
# recomplauncher.json's settings file and folders are checked the way the launcher picks them (the first entry whose
# "if" exists).
#
#   python3 launcher/tests/recomplauncher_test.py EXTRACTED_APPIMAGE_DIR
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MARKER = '.country-box-data-copied'
failures = 0


def check(cond, what):
    global failures
    print(('ok    ' if cond else 'FAIL  ') + what)
    failures += not cond


def install(case, extracted):
    """The game's folder as RecompLauncher leaves it: the AppImage's files, under a library folder."""
    lib = case / 'Games'
    lib.mkdir(parents=True)
    game = lib / 'Country-Box'
    shutil.copytree(extracted, game, symlinks=True)
    return lib, game, game / 'recompdata'


def env_for(home, data, **extra):
    env = {k: v for k, v in os.environ.items()
           if k not in ('XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_CONFIG_HOME', 'CB_LAUNCHER_HOME', 'CB_DESKTOP_DIR',
                        'APPIMAGE', 'APPDIR', 'TMPDIR')}
    env.update(HOME=str(home), CB_LAUNCHER_NO_UPDATE='1', SDL_VIDEO_DRIVER='dummy', RECOMPLAUNCHER='1',
               RECOMPLAUNCHER_DATA=str(data))
    env.update(extra)
    return env


def start(game, env, *args):
    """One start of Country-Box (--status by default): rc, the status, stderr."""
    p = subprocess.run([str(game / 'AppRun'), *(args or ('--status',))], env=env, capture_output=True, text=True,
                       timeout=600, cwd=str(game.parent.parent))
    try:
        return p.returncode, json.loads(p.stdout), p.stderr
    except ValueError:
        return p.returncode, {}, p.stderr + p.stdout


def old_data(d):
    """What 1.0.9 left in <library>/Country-Box-data: both games' saves and settings, a build, logs, caches."""
    for g, ini in (('dkctf', 'fullscreen = 1\n'), ('dkcrhd', 'fps_limit = display\n')):
        b = d / 'games' / g
        (b / 'profile' / 'save').mkdir(parents=True)
        (b / 'profile' / 'save' / 'slot0.bin').write_bytes(g.encode() + b' save \x00\x01')
        (b / 'mp4.ini').write_text(ini)
    exe = d / 'games' / 'dkctf' / 'country-box'
    exe.write_bytes(b'\x7fELF' + b'\0' * 4096)
    exe.chmod(0o755)
    (d / 'logs').mkdir()
    (d / 'logs' / 'play.log').write_text('log\n')
    (d / 'tmp').mkdir()
    (d / 'tmp' / 'junk').write_text('x')
    (d / 'cache').mkdir()
    (d / 'cache' / 'junk').write_text('x')
    (d / 'library.ini').write_text(f'[dkctf]\nbuild = {d}/games/dkctf\n')


def saves_ok(data, what):
    ok = all((data / 'games' / g / 'profile' / 'save' / 'slot0.bin').read_bytes() == g.encode() + b' save \x00\x01'
             for g in ('dkctf', 'dkcrhd') if (data / 'games' / g / 'profile' / 'save' / 'slot0.bin').is_file())
    ok = ok and all((data / 'games' / g / 'profile' / 'save' / 'slot0.bin').is_file() for g in ('dkctf', 'dkcrhd'))
    check(ok, f'{what}: both games\' saves are in {{data}}, byte for byte')


def leftovers(d):
    return [str(p.relative_to(d)) for p in d.rglob('*.copying')]


# ---- recomplauncher.json, picked the way the launcher picks it (profile.rs place(): the first entry whose "if" exists)

MANIFEST = json.loads((ROOT / 'recomplauncher.json').read_text())


def fill(t, game, home):
    return (t.replace('{data}', str(game / 'recompdata')).replace('{install}', str(game))
            .replace('{local}', str(home / '.local' / 'share')))


def place(entries, game, home):
    for e in entries:
        if not e.get('if') or os.path.exists(fill(e['if'], game, home)):
            return Path(os.path.normpath(fill(e['path'], game, home)))
    return None


def settings_file(gid, game, home):
    return place(next(c for c in MANIFEST['config'] if c['id'] == gid)['file'], game, home)


def main():
    extracted = Path(sys.argv[1]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix='cb-recomplauncher-test-'))
    try:
        # 1. a new player: everything in {data}, nothing beside the game's folder or in the home
        case = tmp / 'fresh'
        home = case / 'home'
        home.mkdir(parents=True)
        lib, game, data = install(case, extracted)
        check(settings_file('dkctf', game, home) == data / 'games' / 'dkctf' / 'mp4.ini',
              'new player: the launcher\'s settings go to {data}')
        rc, st, err = start(game, env_for(home, data))
        check(rc == 0 and st.get('home') == str(data), f'new player: the library is {{data}} ({err.strip() or "ok"})')
        check((data / 'tmp').is_dir() and not (lib / 'Country-Box-data').exists() and not (home / '.local').exists(),
              'new player: temporary files in {data}; nothing beside the game\'s folder or in the home')
        check(not (data / MARKER).exists(), 'new player: nothing to copy, no marker')

        # 2. an old Country-Box-data beside the game's folder: the launcher edits it until the first start, which
        #    copies it in (once); then the launcher edits {data}'s
        case = tmp / 'old'
        home = case / 'home'
        home.mkdir(parents=True)
        lib, game, data = install(case, extracted)
        old = lib / 'Country-Box-data'
        old_data(old)
        check(settings_file('dkctf', game, home) == old / 'games' / 'dkctf' / 'mp4.ini'
              and place(MANIFEST['folders']['saves'], game, home) == old / 'games',
              'old data, before the first start: the launcher\'s settings and Saves folder are the old ones')
        check(not (data / 'games').exists(), 'old data, before the first start: {data}/games is not made')
        snapshot = {str(p.relative_to(old)): p.read_bytes() for p in old.rglob('*') if p.is_file()}
        rc, st, err = start(game, env_for(home, data))
        check(rc == 0 and st.get('home') == str(data) and (data / MARKER).is_file(),
              f'old data: copied in, the library is {{data}}, the marker is written ({err.strip() or "ok"})')
        saves_ok(data, 'old data')
        check((data / 'games' / 'dkctf' / 'mp4.ini').read_text() == 'fullscreen = 1\n'
              and (data / 'logs' / 'play.log').is_file() and os.access(data / 'games' / 'dkctf' / 'country-box', os.X_OK),
              'old data: settings, logs and the build came along')
        check(not (data / 'cache' / 'junk').exists() and not (data / 'tmp' / 'junk').exists(),
              'old data: temporary files and caches are not copied')
        check(f'build = {data}/games/dkctf' in (data / 'library.ini').read_text(), 'old data: library.ini build= follows')
        check({str(p.relative_to(old)): p.read_bytes() for p in old.rglob('*') if p.is_file()} == snapshot,
              'old data: the old folder is left as it was')
        check(settings_file('dkctf', game, home) == data / 'games' / 'dkctf' / 'mp4.ini'
              and place(MANIFEST['folders']['saves'], game, home) == data / 'games'
              and place(MANIFEST['folders']['logs'], game, home) == data / 'logs',
              'old data, after the copy: the launcher\'s settings, Saves and Logs are {data}\'s')
        # a finished copy isn't repeated: later changes in the old folder stay there, {data}'s own stay
        (data / 'games' / 'dkctf' / 'mp4.ini').write_text('fullscreen = 0\n')
        (old / 'games' / 'dkctf' / 'profile' / 'save' / 'slot1.bin').write_bytes(b'later')
        rc, st, _ = start(game, env_for(home, data))
        check(rc == 0 and st.get('home') == str(data) and (data / 'games' / 'dkctf' / 'mp4.ini').read_text() == 'fullscreen = 0\n'
              and not (data / 'games' / 'dkctf' / 'profile' / 'save' / 'slot1.bin').exists(),
              'old data, the next start: nothing is copied again, {data}\'s settings stay')

        # 3. the launcher wrote {data}/games/<game>/mp4.ini before the first start (a Game Setting changed right after
        #    the update): the copy goes into that folder, the saves arrive, and the old settings file wins
        case = tmp / 'launcher-first'
        home = case / 'home'
        home.mkdir(parents=True)
        lib, game, data = install(case, extracted)
        old = lib / 'Country-Box-data'
        old_data(old)
        (data / 'games' / 'dkctf').mkdir(parents=True)
        (data / 'games' / 'dkctf' / 'mp4.ini').write_text('fullscreen = 0\n')
        rc, st, err = start(game, env_for(home, data))
        check(rc == 0 and st.get('home') == str(data) and (data / MARKER).is_file(),
              f'launcher-written settings first: the copy still finishes ({err.strip() or "ok"})')
        saves_ok(data, 'launcher-written settings first')
        check((data / 'games' / 'dkctf' / 'mp4.ini').read_text() == 'fullscreen = 1\n'
              and (data / 'games' / 'dkcrhd' / 'mp4.ini').read_text() == 'fps_limit = display\n',
              'launcher-written settings first: the old mp4.ini wins on the adoption start')
        check(os.access(data / 'games' / 'dkctf' / 'country-box', os.X_OK) and not leftovers(data),
              'launcher-written settings first: the rest of that game\'s folder came too; no .copying left')

        # 4. a copy cut short: leftovers from an earlier start, and a folder in {data} that can't be written. That
        #    start stays on the old folder (no marker); the next one cleans up and finishes
        case = tmp / 'interrupted'
        home = case / 'home'
        home.mkdir(parents=True)
        lib, game, data = install(case, extracted)
        old = lib / 'Country-Box-data'
        old_data(old)
        (data / 'logs.copying').mkdir(parents=True)
        (data / 'logs.copying' / 'half').write_text('half')
        g = data / 'games' / 'dkctf'
        (g / 'profile.copying' / 'save').mkdir(parents=True)
        (g / 'profile.copying' / 'save' / 'slot0.bin').write_bytes(b'half')
        (g / 'mp4.ini').write_text('fullscreen = 0\n')
        g.chmod(0o555)
        rc, st, err = start(game, env_for(home, data))
        check(st.get('home') == str(old) and not (data / MARKER).exists() and 'failed' in err,
              f'interrupted: that start keeps the old folder, no marker, it says why ({err.strip()[-160:]})')
        check((old / 'games' / 'dkctf' / 'mp4.ini').read_text() == 'fullscreen = 1\n',
              'interrupted: the old folder is untouched')
        g.chmod(0o755)
        rc, st, err = start(game, env_for(home, data))
        check(rc == 0 and st.get('home') == str(data) and (data / MARKER).is_file(),
              f'interrupted, the next start: the copy finishes ({err.strip() or "ok"})')
        saves_ok(data, 'interrupted, the next start')
        check(not leftovers(data) and (data / 'games' / 'dkctf' / 'mp4.ini').read_text() == 'fullscreen = 1\n'
              and not (data / 'logs' / 'half').exists(),
              'interrupted, the next start: the half copies are gone; the old settings won')

        # 5. Move library: RecompLauncher keeps the library in {data} (its own Move moves the whole game)
        rc, out, err = start(game, env_for(home, data), '--move-library', str(case / 'elsewhere'))
        check(rc == 1 and 'RecompLauncher' in err and not (case / 'elsewhere').exists() and (data / 'games').is_dir(),
              'Move library under RecompLauncher: refused, nothing moved')
    finally:
        for p in tmp.rglob('*'):
            if p.is_dir() and not p.is_symlink():
                p.chmod(0o755)
        shutil.rmtree(tmp, ignore_errors=True)
    print('recomplauncher test:', 'passed' if not failures else f'{failures} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
