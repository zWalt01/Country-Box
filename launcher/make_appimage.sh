#!/bin/sh
# Country-Box: arma la AppImage del launcher que se sube a cada release:
#   AppRun, country-box.desktop, country-box.png (.DirIcon)  launcher/appimage/
#   recomplauncher.json                                      la raíz del repo (RecompLauncher la lee en /recomplauncher.json)
#   usr/bin/country-box-launcher                             launcher/build.py, en un contenedor ubuntu:22.04
#   usr/lib/libSDL3.so.0                                     SDL $SDL_TAG compilada en ese contenedor
#                                                            (appimage/container-build.sh; docker o podman)
#   usr/share/country-box/prep/                              launcher/make_prep_payload.py (traductor: core/build/xlate)
# Nada derivado de un juego entra (ver make_prep_payload.py).
#
#   sh launcher/make_appimage.sh [OUT_DIR]     (por defecto build/; necesita appimagetool en el PATH o en ~/.local/bin)
#
# El launcher y SDL se arman en ubuntu:22.04 (glibc 2.35) para que la AppImage corra en cualquier distribución
# con esa glibc o más nueva; armados en esta máquina pedirían su glibc (Arch: 2.43) y no arrancarían en Ubuntu,
# Debian, Mint ni SteamOS. Al final se revisa: nada dentro puede pedir más de GLIBC_2.35.
# CB_APPIMAGE_LOCAL=1 arma con el sistema (sólo para pruebas en esta máquina; no sale en una release).
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
SDL_TAG=${SDL_TAG:-release-3.4.18}
if [ "${CB_APPIMAGE_LOCAL:-}" = 1 ]; then
    python3 "$ROOT/launcher/build.py" --out "$OUT/appimage/launcher"
    cp "$OUT/appimage/launcher/country-box-launcher" "$APPDIR/usr/bin/"
    cp -L "$(pkg-config --variable=libdir sdl3)/libSDL3.so.0" "$APPDIR/usr/lib/"
else
    ENGINE=$(command -v docker || command -v podman || true)
    [ -n "$ENGINE" ] || { echo "make_appimage: hace falta docker o podman (o CB_APPIMAGE_LOCAL=1 para una prueba local)" >&2; exit 1; }
    mkdir -p "$OUT/appimage/container"
    "$ENGINE" run --rm -v "$ROOT:/src:ro" -v "$OUT/appimage/container:/out" -e SDL_TAG="$SDL_TAG" \
        -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" ubuntu:22.04 sh /src/launcher/appimage/container-build.sh
    cp "$OUT/appimage/container/country-box-launcher" "$APPDIR/usr/bin/"
    cp "$OUT/appimage/container/libSDL3.so.0" "$APPDIR/usr/lib/"
fi
python3 "$ROOT/launcher/make_prep_payload.py" "$APPDIR/usr/share/country-box/prep"
# la garantía: ningún programa o biblioteca de la AppImage pide una glibc más nueva que 2.35
if [ "${CB_APPIMAGE_LOCAL:-}" != 1 ]; then
    worst=$(find "$APPDIR" -type f \( -perm -u+x -o -name '*.so*' \) -exec sh -c 'objdump -T "$1" 2>/dev/null' _ {} \; |
            grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)
    [ "$(printf '%s\nGLIBC_2.35\n' "$worst" | sort -V | tail -1)" = GLIBC_2.35 ] ||
        { echo "make_appimage: algo dentro pide $worst (el máximo es GLIBC_2.35)" >&2; exit 1; }
    echo "glibc: lo más nuevo que se pide es $worst"
fi
cp "$ROOT/launcher/appimage/AppRun" "$ROOT/launcher/appimage/country-box.desktop" "$ROOT/launcher/appimage/country-box.png" "$APPDIR/"
chmod -R a+rX "$APPDIR"
chmod 755 "$APPDIR/AppRun" "$APPDIR/usr/bin/country-box-launcher"
ln -s country-box.png "$APPDIR/.DirIcon"
cp "$ROOT/recomplauncher.json" "$APPDIR/recomplauncher.json"
chmod 644 "$APPDIR/recomplauncher.json"   # the repo keeps its files 0600; other users must read it

IMG=$OUT/Country-Box-$VERSION-linux-x86_64.AppImage
ARCH=x86_64 "$TOOL" --no-appstream "$APPDIR" "$IMG"
echo "built $IMG"
