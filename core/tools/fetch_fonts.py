#!/usr/bin/env python3
# Country-Box: fetches the open fonts the runtime offers a game that asks for a system font (runtime/ov_system.c).
"""Download the open fonts (SIL OFL 1.1) from the official google/fonts repository at a pinned commit, check each
file's SHA-256 against the value pinned below, and write them with their licences into a fonts directory.

    python3 tools/fetch_fonts.py [--out DIR]        default DIR: $CB_FONTS_CACHE or ~/.cache/country-box/fonts
    python3 tools/fetch_fonts.py --print-hashes     download without checking and print the SHA-256 lines to pin

The fonts are not part of the repository (public repo, no file over 1 MiB): tools/build_linux.py copies DIR to
<build>/fonts next to the binary, and the runtime looks there (CB_FONTS_DIR, else <executable directory>/fonts).

  Standard (Latin and Japanese)   BIZ UDPGothic Regular   (Morisawa; the closest open relative of the UD gothic of
                                                          console UI text)
  Chinese Simplified              Noto Sans SC Regular
  Chinese Traditional             Noto Sans TC Regular
  Korean                          Noto Sans KR Regular

The Noto Sans CJK files are variable fonts whose default instance is Thin (wght 100): this script cuts the Regular
(wght 400) instance out of the pinned download with fontTools (pip install fonttools) and keeps only that.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.request
from pathlib import Path

COMMIT = '35e77c3854970a550d9894a85aba21420813a019'   # google/fonts @ 2026-11-12
BASE = f'https://raw.githubusercontent.com/google/fonts/{COMMIT}/ofl'

# (family directory in google/fonts, remote file, local name, SHA-256 of the remote file, wght to cut or None)
FONTS = [
    ('bizudpgothic', 'BIZUDPGothic-Regular.ttf', 'BIZUDPGothic-Regular.ttf',
     '258d7156c165f2ff774b6efee637c22c3b950de0d8a10e501137061bc8085d01', None),
    ('notosanssc', 'NotoSansSC[wght].ttf', 'NotoSansSC-Regular.ttf',
     'a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da', 400),
    ('notosanstc', 'NotoSansTC[wght].ttf', 'NotoSansTC-Regular.ttf',
     '864727d210d54f2537bbe23b3a839436c3992af72de9322af5270897246bd44f', 400),
    ('notosanskr', 'NotoSansKR[wght].ttf', 'NotoSansKR-Regular.ttf',
     '194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252', 400),
]
# SHA-256 of OFL.txt per family (the licence is saved as OFL-<family>.txt next to the fonts).
LICENSES = {
    'bizudpgothic': 'e753d7155d53c747d037a445e584c8ecfca6dd79846db610417e282a736b28bc',
    'notosanssc': '1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9',
    'notosanstc': '1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9',
    'notosanskr': '1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9',
}


def get(url):
    req = urllib.request.Request(url, headers={'User-Agent': 'country-box-fetch-fonts'})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cut_instance(data, wght, dest):
    try:
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
        import io
    except ImportError:
        sys.exit('fetch_fonts: fontTools is needed to cut the Regular instance out of the variable fonts '
                 '(pip install fonttools)')
    font = instancer.instantiateVariableFont(TTFont(io.BytesIO(data)), {'wght': wght}, inplace=False,
                                             updateFontNames=False)
    font.save(dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=Path(os.environ.get('CB_FONTS_CACHE')
                                                     or Path.home() / '.cache' / 'country-box' / 'fonts'))
    ap.add_argument('--print-hashes', action='store_true')
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    manifest_path = a.out / 'sources.json'   # local name -> SHA-256 of the download it came from
    manifest = json.loads(manifest_path.read_text()) if manifest_path.is_file() else {}
    for fam, remote, local, want, wght in FONTS:
        dest = a.out / local
        if dest.is_file() and manifest.get(local) == want and not a.print_hashes:
            print(f'{local}: present (from the pinned download)')
        else:
            data = get(f'{BASE}/{fam}/{remote.replace("[", "%5B").replace("]", "%5D")}')
            got = sha(data)
            if a.print_hashes:
                print(f"    ('{fam}', '{remote}', '{local}',\n     '{got}', {wght}),")
            elif got != want:
                sys.exit(f'{local}: SHA-256 {got}, pinned {want}; not written')
            if wght is None:
                dest.write_bytes(data)
            else:
                cut_instance(data, wght, dest)
            manifest[local] = got
            print(f'{local}: {dest.stat().st_size:,} bytes')
        lic = a.out / f'OFL-{fam}.txt'
        if not lic.is_file() or sha(lic.read_bytes()) != LICENSES[fam]:
            text = get(f'{BASE}/{fam}/OFL.txt')
            if sha(text) != LICENSES[fam] and not a.print_hashes:
                sys.exit(f'OFL-{fam}.txt: SHA-256 {sha(text)}, pinned {LICENSES[fam]}; not written')
            lic.write_bytes(text)
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    print(f'fonts in {a.out}')


if __name__ == '__main__':
    main()
