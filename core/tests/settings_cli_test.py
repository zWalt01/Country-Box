#!/usr/bin/env python3
# Country-Box: test of the settings command line (runtime/settings.c) the launcher uses. Runs a built binary against a
# scratch settings file (CB_SETTINGS_FILE), so the build's own mp4.ini is never touched:
#   python3 core/tests/settings_cli_test.py <build>/country-box
# Checks: the dump is JSON with groups -> rows, each row has the launcher's fields; --settings-set stores public values
# as their cfg values and keeps the rest of the file; an unavailable option, an unknown key or a bad value is refused
# and nothing at all is written; "default" removes the key; the Steam Deck defaults; the in-game menu's file format.
import json
import os
import subprocess
import sys
import tempfile

ROW_FIELDS = {'key', 'label', 'description', 'type', 'value', 'default', 'restart', 'set_by_env', 'unavailable', 'cfg'}
fails = 0


def check(ok, what):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + what)
    fails += not ok


def run(exe, ini, *args, machine='pc'):
    env = {k: v for k, v in os.environ.items() if not k.startswith('MP4_')}
    env.update(CB_SETTINGS_FILE=ini, CB_MACHINE=machine)
    p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=60)
    try:
        out = json.loads(p.stdout)
    except ValueError:
        out = None
    return p.returncode, out, p.stdout


def rows(dump):
    return {r['key']: r for g in dump['groups'] for r in g['rows']}


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else 'country-box'
    d = tempfile.mkdtemp(prefix='cb-settings-test-')
    ini = os.path.join(d, 'mp4.ini')
    with open(ini, 'w') as f:
        f.write('# kept as it is\n[audio]\nvolume = 70\n')

    code, dump, raw = run(exe, ini, '--settings-dump')
    check(code == 0 and dump is not None, 'dump exits 0 with JSON on stdout')
    if dump is None:
        print(raw[:2000])
        return 1
    check(dump.get('format') == 1 and dump.get('game') in ('dkcr-hd', 'dkctf'), 'dump names its format and game')
    check(dump.get('file') == ini, 'dump names the settings file (CB_SETTINGS_FILE)')
    root_version = open(os.path.join(os.path.dirname(__file__), '..', '..', 'VERSION')).read().strip()
    check(dump.get('version') == root_version, f'dump gives the Country-Box version ({root_version}, VERSION)')
    check(all('short_label' in row for g in dump['groups'] for row in g['rows']), 'every row has a short label')
    r = rows(dump)
    check(all(ROW_FIELDS <= set(row) for row in r.values()), 'every row has the launcher fields')
    check(all(('options' in row) == (row['type'] == 'choice') for row in r.values()), 'choice rows list options')
    check(all({'min', 'max', 'step', 'unit'} <= set(row) for row in r.values() if row['type'] == 'range'),
          'range rows give min, max, step, unit')
    for key in ('video.display_mode', 'video.window_size', 'video.upscaler', 'video.sharpness', 'video.aspect',
                'video.anisotropy', 'video.vsync', 'video.refresh', 'video.perf_overlay', 'sound.volume'):
        check(key in r, f'{key} is in the model')
    check(r['sound.volume']['value'] == '70', 'the file\'s value is the row\'s value (volume 70)')
    ref = {o['value']: o for o in r['video.refresh']['options']}
    check(ref['60']['unavailable'] is None and ref['90']['unavailable'] and ref['variable']['unavailable'],
          'refresh: 60 available, 90 and variable unavailable with a reason')
    check(r['video.display_mode']['default'] == 'windowed' and r['video.window_size']['default'] == '1920x1080',
          'PC defaults: windowed, 1920x1080')

    code, out, _ = run(exe, ini, '--settings-set', 'video.display_mode=borderless', 'video.sharpness=40',
                       'video.upscaler=bilinear')
    check(code == 0 and out and out['ok'] and out['saved'], 'set three valid pairs')
    text = open(ini).read()
    check('fullscreen = 1' in text and 'sharpness = 40' in text and 'upscaler = bilinear' in text,
          'stored as cfg values (fullscreen = 1, sharpness = 40, upscaler = bilinear)')
    check(text.startswith('# kept as it is\n') and 'volume = 70' in text, 'the rest of the file is kept')
    _, dump, _ = run(exe, ini, '--settings-dump')
    check(rows(dump)['video.display_mode']['value'] == 'borderless', 'the dump reads the set value back')

    before = open(ini).read()
    for pairs, what in ((['video.refresh=90'], 'an unavailable option'),
                        (['video.aspect=stretch', 'video.nothing=1'], 'an unknown key next to a valid one'),
                        (['video.sharpness=400'], 'a number out of range'),
                        (['video.vsync=sometimes'], 'a value not in the list')):
        code, out, _ = run(exe, ini, '--settings-set', *pairs)
        check(code == 2 and out and not out['ok'] and out['errors'], f'{what} is refused')
        check(open(ini).read() == before, f'{what}: nothing written')

    code, out, _ = run(exe, ini, '--settings-set', 'video.sharpness=default')
    check(code == 0 and 'sharpness' not in open(ini).read(), '"default" removes the key')

    os.remove(ini)
    _, dump, _ = run(exe, ini, '--settings-dump', machine='deck-oled')
    r = rows(dump)
    check(dump['machine'] == 'deck-oled' and r['video.display_mode']['value'] == 'borderless' and
          r['video.window_size']['value'] == '1280x800', 'Steam Deck OLED defaults: borderless, 1280x800')
    print('settings_cli_test: %s (%d failures)' % ('FAILED' if fails else 'passed', fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
