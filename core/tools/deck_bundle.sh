#!/usr/bin/env bash
# Country-Box: the folder for a Steam Deck test session, built from a tools/build_linux.py build. Holds the binary, the
# shader translator, the RomFS index, the platform and memory checks and run.sh; the game's exefs/ and romfs/ go next to
# them on the Deck (main.c portable_paths finds them there), copied from the player's dump, never put in the bundle by
# this script. Usage:
#   tools/deck_bundle.sh <build dir> <generated dir> <bundle dir>
set -euo pipefail
BUILD=${1:?build dir}; GEN=${2:?generated dir}; OUT=${3:?bundle dir}
cd "$(dirname "$0")/.."
mkdir -p "$OUT"
cp -f "$BUILD/country-box" "$BUILD/mp4_xlate" "$BUILD/mp4_xlate.id" "$BUILD/romfs-index.bin" "$OUT/"
# the shader and pipeline lists (no game code) the first start prepares from (runtime/shader_prep.c)
for f in shader-list.bin pipeline-list.bin shader-patches.bin; do if [ -f "$BUILD/$f" ]; then cp -f "$BUILD/$f" "$OUT/"; fi; done
# the level sweep (tools/sweep.py, --build is the bundle itself) and the save its menus start from (a Game 1 save)
cp -f tools/sweep.py "$OUT/"
cp -f tools/cb-job.sh "$OUT/" && chmod +x "$OUT/cb-job.sh"
if [ -d "$BUILD/profile" ]; then rm -rf "$OUT/profile"; cp -a "$BUILD/profile" "$OUT/profile"; fi
clang -std=gnu17 -O2 -march=x86-64-v3 -Iruntime tests/platform_check.c runtime/platform/linux.c \
    -o "$OUT/platform_check" -lpthread
clang -std=gnu17 -O1 -march=x86-64-v3 -Iruntime -I"$GEN" tests/memory_check.c runtime/memory.c \
    runtime/platform/linux.c -o "$OUT/memory_check" -lpthread
clang -std=gnu17 -O1 tests/userptr_wp_check.c -lvulkan -o "$OUT/userptr_wp_check"
# openh264 for the cinematics (ov_movie.c OPENH264_LIB, looked for next to the executable first): this machine's copy,
# for the player's own Deck session; SteamOS is not known to ship it.
OH=$(ldconfig -p | awk '/libopenh264\.so\.8 /{print $NF; exit}')
if [ -n "$OH" ]; then cp -fL "$OH" "$OUT/libopenh264.so.8"; else echo "warning: libopenh264.so.8 not found: no cinematics" >&2; fi
cat > "$OUT/run.sh" <<'EOF'
#!/usr/bin/env bash
# Country-Box Deck session: checks, boot, title screen with frame times. Logs and shots in ./session-<time>/.
cd "$(dirname "$0")"
S=session-$(date +%Y%m%d-%H%M%S); mkdir -p "$S"
uname -a > "$S/system.txt"; cat /etc/os-release >> "$S/system.txt"; ldd --version | head -1 >> "$S/system.txt"
ls -l /dev/udmabuf >> "$S/system.txt" 2>&1; cat /proc/sys/vm/unprivileged_userfaultfd >> "$S/system.txt" 2>&1
cat /sys/kernel/mm/transparent_hugepage/enabled >> "$S/system.txt" 2>&1; ulimit -e >> "$S/system.txt"
./platform_check > "$S/platform_check.log" 2>&1; echo "platform_check: $(tail -1 "$S/platform_check.log")"
./memory_check > "$S/memory_check.log" 2>&1; echo "memory_check: $(tail -1 "$S/memory_check.log")"
./userptr_wp_check > "$S/userptr_wp_check.log" 2>&1; echo "userptr_wp_check: $(tail -1 "$S/userptr_wp_check.log")"
./country-box --probe-kernel > "$S/probe-kernel.log" 2>&1; echo "probe-kernel: $(tail -1 "$S/probe-kernel.log")"
./country-box --boot > "$S/boot.log" 2>&1; echo "boot: $(tail -1 "$S/boot.log")"
MP4_SHOTS=w0-60:10 MP4_SHOTS_DIR="$S" ./country-box --seconds 60 --shot "$S/title.png" > "$S/title.log" 2>&1
echo "title: $(grep 'frametime: exit' "$S/title.log")"
echo "session in $S"
EOF
cat > "$OUT/sweep.sh" <<'EOF'
#!/usr/bin/env bash
# Country-Box Deck sweep: every level (or the ones named) played by Super Guide, measured in ms per frame and per
# thread with the APU's watts. Usage: ./sweep.sh [levels, e.g. 12,14,minecart] [menu scale, default 1.0]
cd "$(dirname "$0")"
O=sweep-$(date +%Y%m%d-%H%M%S)
python3 sweep.py --build . --out "$O" ${1:+--levels "$1"} --menu-scale "${2:-1.0}" --resume
echo "report in $O/report.md"
EOF
chmod +x "$OUT/sweep.sh"
chmod +x "$OUT/run.sh"
echo "bundle in $OUT ($(du -sh "$OUT" | cut -f1)); on the Deck add exefs/ and romfs/ from the dump next to run.sh"
