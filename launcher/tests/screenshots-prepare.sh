#!/bin/sh
# Country-Box: screenshots of the Prepare milestone — the card's PREPARE and "Import a build" buttons, the
# prepare screen mid-run (a real pipeline: lift + compile + shaders on a real copy, at the Steam Deck's 1280x800
# and at 1920x1080), the finished state, the import browser and the imported build. Scratch home; the real
# library is never touched.
#
#   [GAME=dkctf] launcher/tests/screenshots-prepare.sh OUT COPY PAYLOAD PYTHON
#
# COPY: an extracted copy of GAME (exefs/ + romfs/; GAME defaults to dkcrhd — dkctf is the smaller run);
# PAYLOAD: a prep/ payload (launcher/make_prep_payload.py); PYTHON: a python3 with capstone/unicorn/lz4/numpy
# (the lifter's). CB_PREP_JOBS sets -j for the real run.
set -eu
out=$1 rcopy=$2 payload=$3 pypath=$4 game=${GAME:-dkcrhd}
here=$(cd "$(dirname "$0")/.." && pwd)
exe=$(cd "$here/.." && pwd)/build/launcher/country-box-launcher
mkdir -p "$out"

# a build made "elsewhere": the fixture is this script's own stub with a stamp matching the copy
import_src=$(mktemp -d /tmp/cb-import-src.XXXXXX)
home=$(mktemp -d /tmp/cb-launcher-home.XXXXXX)
export CB_LAUNCHER_HOME="$home" CB_SETTINGS_FILE="$home/settings.ini" SDL_AUDIO_DRIVER=dummy
export CB_PREP_SOURCES="$payload" CB_PYTHON="$pypath"
"$exe" --add $game "$rcopy" --quick > /dev/null
mid=$(sed -n 's/^copy_manifest = //p' "$home/library.ini")
mkdir -p "$import_src/build"
printf '#!/bin/sh\necho stub\n' > "$import_src/build/country-box"
chmod +x "$import_src/build/country-box"
printf 'test-xlate\n' > "$import_src/build/mp4_xlate.id"
printf '{"country-box":"0.1.0","game":"%s","copy_manifest":"%s","toolchain":"clang test"}\n' "$game" "$mid" \
    > "$import_src/build/build-stamp.json"

for size in 1280x800 1920x1080; do
    w=${size%x*} h=${size#*x}
    # the card with PREPARE + Import a build (copy checked, no build yet)
    steps="frames:25;pad;frames:2;shot:$size-1-card-prepare"
    # the import browser on that card
    steps="$steps;key:down;frames:3;shot:$size-2-import-button"
    # import: browse to the fixture, use it, done
    steps="$steps;key:b;frames:5;import:$game:$import_src/build;frames:12;pad;frames:2;shot:$size-3-import-browse"
    steps="$steps;use-folder;frames:40;wait-prep;frames:5;pad;frames:2;shot:$size-4-import-done"
    # the imported build leaves the game needing shaders (stub): back to the hub shows the card again
    steps="$steps;key:a;frames:8;pad;frames:2;shot:$size-5-card-after-import"
    nice -n 19 gamescope --backend headless -W "$w" -H "$h" -- "$exe" --shot-dir "$out" --auto "$steps" 2>&1 | grep '^shot' || true
done

# the real pipeline, once, at 1280x800: progress mid-lift/mid-compile and the finished screen
rm -rf "$home/games/$game" "$home/gen"
steps="prepare:$game;wait-stage:2:20;pad;frames:2;shot:1280x800-6-prepare-lift"
steps="$steps;wait-stage:3:60;pad;frames:2;shot:1280x800-7-prepare-compile"
steps="$steps;wait-prep;frames:8;pad;frames:2;shot:1280x800-8-prepare-done;key:a;frames:10"
steps="$steps;pad;frames:2;shot:1280x800-9-card-ready"
nice -n 19 gamescope --backend headless -W 1280 -H 800 -- "$exe" --shot-dir "$out" --auto "$steps" 2>&1 | grep '^shot' || true
rm -rf "$home" "$import_src"
