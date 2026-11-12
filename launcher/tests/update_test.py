#!/usr/bin/env python3
# Country-Box: test of the launcher's update channel (country-box-launcher --check-update / --update) against a local
# HTTP server that plays the GitHub Releases API — never the real one. Cases: a full install (download, SHA-256,
# atomic swap, saves backup, needs-preparing), a corrupted download (wrong hash), an interrupted download (the server
# cuts the transfer), an up-to-date release, a release without any published hash, and the SHA256SUMS variant.
#
#   python3 launcher/tests/update_test.py build/launcher/country-box-launcher
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

fails = 0


def check(ok, what):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + what)
    fails += not ok


class FakeReleases(BaseHTTPRequestHandler):
    """Serves /repos/o/r/releases/latest and /dl/<asset>. The release JSON, the asset bytes and how the asset is
    served (whole, other content than hashed, or cut in half) are set from the test."""

    release = {}      # the releases/latest JSON body (dict)
    asset = b''       # the asset's real bytes
    serve = 'whole'   # 'whole' | 'wrong' (other bytes) | 'cut' (half the body, then close)

    def log_message(self, *a):
        pass

    def _send(self, code, body: bytes, ctype='application/octet-stream'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.endswith('/releases/latest'):
            self._send(200, json.dumps(self.release).encode(), 'application/json')
        elif self.path.endswith('/SHA256SUMS'):   # its own asset, not the /dl/ bytes
            text = ''
            for a in self.release.get('assets', []):
                if a['name'] == 'SHA256SUMS':
                    text = a['content']
            self._send(200, text.encode(), 'text/plain')
        elif self.path.startswith('/dl/'):
            if self.serve == 'wrong':
                self._send(200, b'not the package that was hashed')
            elif self.serve == 'cut':
                body = self.asset[:len(self.asset) // 2]
                self.send_response(200)
                self.send_header('Content-Length', str(len(self.asset)))   # promises the whole thing
                self.end_headers()
                self.wfile.write(body)
                self.wfile.flush()
                self.connection.close()                                     # ... and hangs up
            else:
                self._send(200, self.asset)
        else:
            self._send(404, b'{}')


def release(tag, asset_name, asset_bytes, digest=None, with_sums=False):
    # the URLs get the server's real port in set_urls(), once the server is listening
    assets = [{'name': asset_name,
               'browser_download_url': 'http://127.0.0.1:PORT/dl/' + asset_name,
               'url': 'http://127.0.0.1:PORT/dl/' + asset_name,
               'size': len(asset_bytes)}]
    if digest:
        assets[0]['digest'] = digest
    if with_sums:
        assets.append({'name': 'SHA256SUMS',
                       'browser_download_url': 'http://127.0.0.1:PORT/dl/SHA256SUMS',
                       'size': 0})
    return {'tag_name': tag, 'name': f'Country-Box {tag}',
            'body': '- Fixed the barrel art\n- Faster level sweep',
            'assets': assets,
            '_sums': hashlib.sha256(asset_bytes).hexdigest() + '  ' + asset_name + '\n' if with_sums else None}


def set_urls(rel, port):
    for a in rel['assets']:
        for k in ('browser_download_url', 'url'):
            if k in a:
                a[k] = a[k].replace('PORT', str(port))
    return rel


def fill_sums(rel):
    """The handler serves SHA256SUMS from 'content' (the API's real body has no such field; the test reads it here)."""
    for a in rel['assets']:
        if a['name'] == 'SHA256SUMS':
            a['content'] = rel['_sums']
    return rel


VERSION = (Path(__file__).resolve().parents[2] / 'VERSION').read_text().strip()   # the launcher's own version


def make_home(tmp, case, current_version='0.1.0'):
    """A home with a previous install and one game that has saves (profile/, mp4.ini) and a build."""
    home = tmp / ('home-' + case)
    app = home / 'app' / current_version
    app.mkdir(parents=True)
    exe = app / 'country-box-launcher'
    exe.write_bytes(b'#!/bin/sh\n# previous launcher\n')
    exe.chmod(0o755)
    (home / 'app' / 'current').symlink_to(current_version)
    game = home / 'games' / 'dkcrhd'
    game.mkdir(parents=True)
    gexe = game / 'country-box'
    gexe.write_bytes(b'#!/bin/sh\nexit 1\n')   # present and executable: the game is built
    gexe.chmod(0o755)
    (game / 'profile').mkdir()
    (game / 'profile' / 'saves.bin').write_bytes(b'save of the player')
    (game / 'mp4.ini').write_bytes(b'[frame rate]\n# fps_limit = 60\n')
    return home


def run(exe, home, url, *args):
    env = dict(os.environ, CB_LAUNCHER_HOME=str(home), CB_UPDATE_API=url)
    p = subprocess.run([exe, *args], env=env, capture_output=True, text=True, timeout=120)
    try:
        return p.returncode, json.loads(p.stdout)
    except ValueError:
        return p.returncode, None


def main():
    exe = Path(sys.argv[1]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix='cb-update-test-'))
    srv = ThreadingHTTPServer(('127.0.0.1', 0), FakeReleases)
    port = srv.server_address[1]
    url = f'http://127.0.0.1:{port}/repos/o/r/releases/latest'
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        pkg = os.urandom(3 * 1024 * 1024)
        pkg_hash = hashlib.sha256(pkg).hexdigest()
        name = 'country-box-launcher-9.9.9'

        # ---- A: the full update
        home = make_home(tmp, 'a')
        saves_before = hashlib.sha256((home / 'games/dkcrhd/profile/saves.bin').read_bytes()).hexdigest()
        FakeReleases.release = fill_sums(release('v9.9.9', name, pkg, digest='sha256:' + pkg_hash))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'whole'
        code, r = run(exe, home, url, '--check-update')
        check(code == 0 and r and r['ok'] and r['available'] and r['latest'] == '9.9.9' and r['current'] == VERSION,
              f'the check finds 9.9.9 over {VERSION}')
        check(r and r['asset'] and r['asset']['sha256'] == pkg_hash and r['asset']['sha256_from'] == 'release digest',
              'the package hash comes from the release digest')
        check(r and (r.get('notes') or '').startswith('- Fixed'), 'the notes come along')
        code, r = run(exe, home, url, '--update')
        check(code == 0 and r and r['ok'] and r['to'] == '9.9.9' and r['from'] == '0.1.0', 'the update installs')
        check(r and 'dkcrhd' in r['need_prepare'], 'the built game is marked as needing preparing')
        check(r and len(r['saved']) == 1 and 'dkcrhd' in r['saved'][0], 'the saves backup is reported')
        new_exe = home / 'app' / '9.9.9' / 'country-box-launcher'
        check(new_exe.is_file() and new_exe.read_bytes() == pkg and os.access(new_exe, os.X_OK),
              'the new binary is installed, executable and byte-identical to the asset')
        check(os.readlink(home / 'app' / 'current') == '9.9.9', 'app/current points at the new version')
        check((home / 'app' / '0.1.0' / 'country-box-launcher').is_file(), 'the previous version stays for rollback')
        check(not list((home / 'cache').glob('*.part')), 'no partial download is left in the cache')
        backups = sorted((home / 'backups').glob('update-*'))
        check(bool(backups) and (backups[-1] / 'dkcrhd' / 'profile' / 'saves.bin').read_bytes() == b'save of the player'
              and (backups[-1] / 'dkcrhd' / 'mp4.ini').exists(), 'the saves were backed up before installing')
        check(hashlib.sha256((home / 'games/dkcrhd/profile/saves.bin').read_bytes()).hexdigest() == saves_before and
              (home / 'games/dkcrhd/country-box').is_file(), 'the game build and its saves are untouched')

        # ---- B: a corrupted download (the served bytes do not match the published hash)
        home = make_home(tmp, 'b')
        FakeReleases.release = fill_sums(release('v9.9.9', name, pkg, digest='sha256:' + pkg_hash))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'wrong'
        code, r = run(exe, home, url, '--update')
        check(code == 1 and r and not r['ok'] and 'SHA-256' in r['error'], 'a corrupted download is refused by its hash')
        check(not (home / 'app' / '9.9.9').exists() and os.readlink(home / 'app' / 'current') == '0.1.0'
              and not list((home / 'cache').glob('*.part')), 'nothing was installed and no partial file stays')

        # ---- C: an interrupted download (the server cuts the transfer)
        home = make_home(tmp, 'c')
        FakeReleases.release = fill_sums(release('v9.9.9', name, pkg, digest='sha256:' + pkg_hash))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'cut'
        code, r = run(exe, home, url, '--update')
        check(code == 1 and r and not r['ok'] and 'download' in r['error'].lower(), 'an interrupted download fails')
        check(not (home / 'app' / '9.9.9').exists() and os.readlink(home / 'app' / 'current') == '0.1.0'
              and not list((home / 'cache').glob('*.part')), 'nothing was installed and the cache is clean')

        # ---- D: up to date
        home = make_home(tmp, 'd')
        FakeReleases.release = fill_sums(release('v0.0.1', 'country-box-launcher-0.0.1', pkg, digest='sha256:' + pkg_hash))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'whole'
        code, r = run(exe, home, url, '--check-update')
        check(code == 0 and r and r['ok'] and not r['available'] and r['latest'] == '0.0.1', 'an older release is up to date')

        # ---- E: no hash published
        home = make_home(tmp, 'e')
        FakeReleases.release = fill_sums(release('v9.9.9', name, pkg))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'whole'
        code, r = run(exe, home, url, '--check-update')
        check(code == 1 and r and not r['ok'] and 'SHA-256' in r['error'], 'a release without a hash is refused')

        # ---- F: the hash comes from a SHA256SUMS asset instead of the digest
        home = make_home(tmp, 'f')
        FakeReleases.release = fill_sums(release('v9.9.9', name, pkg, with_sums=True))
        set_urls(FakeReleases.release, port)
        FakeReleases.asset, FakeReleases.serve = pkg, 'whole'
        code, r = run(exe, home, url, '--check-update')
        check(code == 0 and r and r['ok'] and r['asset']['sha256_from'] == 'SHA256SUMS', 'SHA256SUMS is accepted too')
        code, r = run(exe, home, url, '--update')
        check(code == 0 and r and r['ok'] and os.readlink(home / 'app' / 'current') == '9.9.9', 'and installs')
    finally:
        shutil.rmtree(tmp)
        srv.shutdown()
    print('update test:', 'passed' if not fails else f'{fails} FAILED')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
