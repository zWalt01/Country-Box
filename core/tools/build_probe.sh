#!/usr/bin/env bash
# Country-Box: builds the Linux kernel probe (tests/kernel_probe_main.c + the ported runtime files + the game's lifted
# objects) until tools/build_game.py has its Linux build. Usage:
#   tools/build_probe.sh <generated dir (layout.h, patches.h)> <lifted objects dir> <out dir>
# The lifted objects must be compiled with -mno-red-zone (runtime/kernel.c kern_ap_stub).
set -euo pipefail
GEN=${1:?generated dir}; OBJS=${2:?lifted objects dir}; OUT=${3:?out dir}
cd "$(dirname "$0")/.."
mkdir -p "$OUT/obj"
FLAGS=(-std=gnu17 -O2 -g -Wall -Wno-unused-function -fno-strict-aliasing -march=x86-64-v3
       -Iruntime -I"$GEN" -Iruntime/third_party -Iruntime/third_party/vma)
SRCS=(runtime/kernel_probe.c runtime/kernel.c runtime/memory.c runtime/loader.c runtime/cfg.c runtime/svc.c
      runtime/dispatch.c runtime/hostfn.c runtime/rt_base.c runtime/game_patch_stubs.c runtime/frame_rate.c
      runtime/patches.c runtime/platform/linux.c tests/kernel_probe_main.c "$OUT/modules.c")
for s in "${SRCS[@]}"; do
    clang "${FLAGS[@]}" -c "$s" -o "$OUT/obj/$(basename "${s%.c}").o"
done
# The lifted objects are linked as one run after the runtime's, so the lifted code's host addresses form one range
# (runtime/kernel.c ap_init checks that no host function falls inside it).
# kernel_probe_spin.c is the preemption test's game code: built like the lifted code and linked between two lifted
# objects, inside that range.
mkdir -p "$OUT/obj-lifted"
clang -O1 -fno-strict-aliasing -march=x86-64-v3 -mno-red-zone -c runtime/kernel_probe_spin.c \
    -o "$OUT/obj-lifted/kernel_probe_spin.o"
LIFTED=("$OBJS"/*.o)
clang -fuse-ld=lld -o "$OUT/kernel_probe" "$OUT"/obj/*.o "${LIFTED[0]}" "$OUT/obj-lifted/kernel_probe_spin.o" \
    "${LIFTED[@]:1}" -lm -lpthread
echo "built $OUT/kernel_probe"
