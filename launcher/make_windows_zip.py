#!/usr/bin/env python3
# Country-Box: packages the Windows release zip, equivalent to make_appimage.sh / make_tarball.sh on Linux.
#
#   Country-Box/country-box-launcher.exe  the launcher window
#   Country-Box/country-box.exe           copy for CLI / shortcut usage
#   Country-Box/SDL3.dll                  SDL 3 runtime
#   Country-Box/openh264.dll              Cisco OpenH264 video codec
#   Country-Box/prep/                     the Prepare payload (lifter, C sources, build tools, manifests)
#   Country-Box/recomplauncher.json       launcher metadata
#   Country-Box/README.txt                quick start guide
#
# Everything Country-Box writes goes into Country-Box-data next to the Country-Box folder.
#
#   python launcher/make_windows_zip.py [OUT_DIR] [--xlate DIR]
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def find_file(names, dirs):
    for d in dirs:
        if not d:
            continue
        p = Path(d)
        for name in names:
            cand = p / name
            if cand.is_file():
                return cand
    return None


def main():
    ap = argparse.ArgumentParser(description='Package Country-Box for Windows (zip).')
    ap.add_argument('out', nargs='?', type=Path, default=ROOT / 'build')
    ap.add_argument('--xlate', type=Path, default=None)
    a = ap.parse_args()

    out_dir = a.out.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    version = (ROOT / 'VERSION').read_text().strip() if (ROOT / 'VERSION').is_file() else '1.0.8'
    stage = out_dir / 'windows-stage' / 'Country-Box'
    if stage.parent.exists():
        shutil.rmtree(stage.parent)
    stage.mkdir(parents=True, exist_ok=True)

    # 1. Launcher binary
    launcher_exe = find_file(['country-box-launcher.exe', 'country-box.exe'],
                             [out_dir / 'launcher', ROOT / 'build' / 'launcher', ROOT / 'bin'])
    if not launcher_exe:
        print('Building country-box-launcher.exe...')
        subprocess.run([sys.executable, str(ROOT / 'launcher' / 'build.py'), '--out', str(out_dir / 'launcher')],
                       check=True)
        launcher_exe = out_dir / 'launcher' / 'country-box-launcher.exe'

    shutil.copy2(launcher_exe, stage / 'country-box-launcher.exe')
    shutil.copy2(launcher_exe, stage / 'country-box.exe')

    # 2. Dependency DLLs: SDL3.dll and openh264.dll
    search_dirs = [
        ROOT / 'build' / 'launcher',
        ROOT / 'toolchain' / 'deps-mingw' / 'bin',
        Path(os.environ.get('MINGW_PREFIX', '')) / 'bin' if os.environ.get('MINGW_PREFIX') else None,
        out_dir / 'launcher',
    ]

    sdl3 = find_file(['SDL3.dll'], search_dirs)
    if sdl3:
        shutil.copy2(sdl3, stage / 'SDL3.dll')
    else:
        print('warning: SDL3.dll not found in search paths', file=sys.stderr)

    h264 = find_file(['openh264.dll', 'openh264-2.7.0-win64.dll', 'openh264-2.6.0-win64.dll'], search_dirs)
    if h264:
        shutil.copy2(h264, stage / 'openh264.dll')
    else:
        print('warning: openh264.dll not found in search paths', file=sys.stderr)

    # 3. Assemble prep/ payload
    prep_dir = stage / 'prep'
    prep_cmd = [sys.executable, str(ROOT / 'launcher' / 'make_prep_payload.py'), str(prep_dir)]
    if a.xlate:
        prep_cmd.extend(['--xlate', str(a.xlate.resolve())])
    subprocess.run(prep_cmd, check=True)

    # 4. Metadata and README
    if (ROOT / 'recomplauncher.json').is_file():
        shutil.copy2(ROOT / 'recomplauncher.json', stage / 'recomplauncher.json')

    readme_text = f"""Country-Box for Windows (version {version}).

Run:   country-box-launcher.exe  (the launcher window)
       country-box.exe --help    (command line options)

Everything Country-Box writes (your library, builds, saves, settings, logs) goes into the folder
Country-Box-data next to this Country-Box folder.
To update, extract the new version over this folder; Country-Box-data is not touched.
"""
    (stage / 'README.txt').write_text(readme_text, encoding='utf-8')

    # 5. Create zip archive
    zip_name = f'Country-Box-{version}-windows-x86_64.zip'
    zip_path = out_dir / zip_name
    print(f'Compressing {zip_path.name}...')
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for p in stage.parent.rglob('*'):
            if p.is_file():
                zf.write(p, p.relative_to(stage.parent))

    # Generic unversioned name matching linux tarball convention
    generic_zip = out_dir / 'Country-Box-windows-x86_64.zip'
    shutil.copy2(zip_path, generic_zip)

    # Checksums
    for z in (zip_path, generic_zip):
        csum = sha256_file(z)
        (out_dir / f'{z.name}.sha256').write_text(f'{csum}  {z.name}\n')

    shutil.rmtree(stage.parent, ignore_errors=True)
    print(f'Built {zip_path} ({zip_path.stat().st_size / 1e6:.1f} MB)')
    print(f'Built {generic_zip} ({generic_zip.stat().st_size / 1e6:.1f} MB)')


if __name__ == '__main__':
    main()
