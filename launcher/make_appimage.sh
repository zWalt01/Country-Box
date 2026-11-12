#!/bin/sh
# Country-Box: arma la AppImage del launcher que se sube a cada release:
#   AppRun, country-box.desktop, country-box.png (.DirIcon)  launcher/appimage/
#   recomplauncher.json                                      la raíz del repo (RecompLauncher la lee en /recomplauncher.json)
#   usr/bin/country-box-launcher                             launcher/build.py
#   usr/lib/libSDL3.so.0                                     la del sistema (pkg-config sdl3)
#   usr/share/country-box/prep/                              launcher/make_prep_payload.py (traductor: core/build/xlate)
# Nada derivado de un juego entra (ver make_prep_payload.py).
#
#   sh launcher/make_appimage.sh [OUT_DIR]     (por defecto build/; necesita appimagetool en el PATH o en ~/.local/bin)
set -eu
umask 022   # la AppImage la corren otros usuarios: appimagetool rechaza carpetas y programas 0700
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build}
VERSION=$(cat "$ROOT/VERSION")
APPDIR=$OUT/appimage/Country-Box.AppDir
TOOL=$(command -v appimagetool || echo "$HOME/.local/bin/appimagetool")
[ -x "$TOOL" ] || { echo "make_appimage: no encuentro appimagetool" >&2; exit 1; }
python3 -m json.tool "$ROOT/recomplauncher.json" >/dev/null   # un recomplauncher.json roto no sale en una release

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share/country-box"
python3 "$ROOT/launcher/build.py" --out "$OUT/appimage/launcher"
cp "$OUT/appimage/launcher/country-box-launcher" "$APPDIR/usr/bin/"
python3 "$ROOT/launcher/make_prep_payload.py" "$APPDIR/usr/share/country-box/prep"
cp -L "$(pkg-config --variable=libdir sdl3)/libSDL3.so.0" "$APPDIR/usr/lib/"
cp "$ROOT/launcher/appimage/AppRun" "$ROOT/launcher/appimage/country-box.desktop" "$ROOT/launcher/appimage/country-box.png" "$APPDIR/"
chmod -R a+rX "$APPDIR"
chmod 755 "$APPDIR/AppRun" "$APPDIR/usr/bin/country-box-launcher"
ln -s country-box.png "$APPDIR/.DirIcon"
cp "$ROOT/recomplauncher.json" "$APPDIR/recomplauncher.json"

IMG=$OUT/Country-Box-$VERSION-linux-x86_64.AppImage
ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$IMG"
echo "built $IMG"
