#!/bin/sh
# Country-Box: the launcher's milestone screenshots, taken inside a headless gamescope (no window on any monitor) at
# the Steam Deck's 1280x800 and at 1920x1080. Uses a scratch library and a scratch settings file, so neither the
# player's library nor any build's mp4.ini is touched.
#
#   launcher/tests/screenshots.sh OUT RETURNS_COPY RETURNS_BUILD TF_COPY BROKEN_COPY
#
# RETURNS_COPY / TF_COPY: extracted copies (exefs/ + romfs/); RETURNS_BUILD: a folder with a built country-box;
# BROKEN_COPY: a copy with files missing (a test fixture outside the repository).
set -eu
out=$1 rcopy=$2 rbuild=$3 tfcopy=$4 broken=$5
here=$(cd "$(dirname "$0")/.." && pwd)
exe=$(cd "$here/.." && pwd)/build/launcher/country-box-launcher
mkdir -p "$out"
for size in 1280x800 1920x1080; do
    w=${size%x*} h=${size#*x}
    home=$(mktemp -d /tmp/cb-launcher-home.XXXXXX)
    export CB_LAUNCHER_HOME="$home" CB_SETTINGS_FILE="$home/settings.ini" SDL_AUDIO_DRIVER=dummy
    "$exe" --add dkcrhd "$rcopy" > /dev/null
    "$exe" --set-build dkcrhd "$rbuild" > /dev/null
    steps="frames:30;pad;frames:2;shot:$size-1-hub"
    steps="$steps;browse:dkctf:$(dirname "$tfcopy");frames:10;pad;frames:2;shot:$size-2-add-browse"
    steps="$steps;add:dkctf:$tfcopy;frames:25;shot:$size-3-add-checking;wait-check;frames:5;pad;frames:2;shot:$size-4-add-ok"
    steps="$steps;key:a;frames:10;add:dkcrhd:$broken;wait-check;frames:5;pad;frames:2;shot:$size-5-add-error"
    steps="$steps;key:b;frames:10;pad;frames:2;shot:$size-6-hub-after"
    steps="$steps;tab:settings;frames:5;key:down;frames:5;key:down;frames:5;key:down;frames:5;shot:$size-7-settings"
    steps="$steps;key:down;frames:5;key:down;frames:5;key:down;frames:5;key:down;frames:5;key:down;frames:5;key:down;frames:5;key:down;frames:5;shot:$size-8-settings-refresh"
    steps="$steps;tab:about;frames:5;shot:$size-9-about"
    nice -n 19 gamescope --backend headless -W "$w" -H "$h" -- "$exe" --shot-dir "$out" --auto "$steps" 2>&1 | grep '^shot' || true
    rm -rf "$home"
done
