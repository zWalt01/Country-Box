#!/bin/sh
# Country-Box: corre DENTRO de un contenedor ubuntu:22.04 (glibc 2.35) que lanza launcher/make_appimage.sh.
# Compila SDL 3 desde su fuente (Ubuntu 22.04 no la empaqueta) y el launcher contra ella, y deja en /out:
#   country-box-launcher  libSDL3.so.0
# Así la AppImage arranca en cualquier distribución con glibc >= 2.35 (Ubuntu 22.04+, Debian 12+, Fedora,
# Arch, SteamOS…) y no sólo en la de la máquina que la armó. SDL abre en ejecución (dlopen) X11, Wayland,
# PipeWire/Pulse/ALSA, D-Bus y el resto, así que lo que falte en una máquina se salta, no impide arrancar.
#
#   /src = el repo (sólo lectura)   /out = salida   SDL_TAG = la versión de SDL   HOST_UID/HOST_GID = dueño de /out
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    ninja-build pkg-config python3 cmake git ca-certificates make wget gnupg lsb-release software-properties-common \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
    libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev \
    libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libwayland-dev \
    libdecor-0-dev wayland-protocols >/dev/null
# clang 19 (apt.llvm.org): el launcher usa #embed; el clang 14 de 22.04 no lo tiene. Sólo cambia el compilador,
# la glibc y libstdc++ siguen siendo las de 22.04.
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh
bash /tmp/llvm.sh 19 >/dev/null
for t in clang clang++ ld.lld; do ln -sf "/usr/bin/$t-19" "/usr/local/bin/$t"; done
git clone -q --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL.git /tmp/SDL
cmake -S /tmp/SDL -B /tmp/SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
cmake --build /tmp/SDL/build >/dev/null
cmake --install /tmp/SDL/build >/dev/null
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
python3 /src/launcher/build.py --out /tmp/launcher
cp /tmp/launcher/country-box-launcher /out/
cp -L /usr/local/lib/libSDL3.so.0 /out/
chown "$HOST_UID:$HOST_GID" /out/country-box-launcher /out/libSDL3.so.0
