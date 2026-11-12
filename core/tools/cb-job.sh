#!/usr/bin/env bash
# Country-Box on the Steam Deck: the program of the non-Steam shortcut. Steam starts it in Game Mode (so the game runs
# as a player's would: gamescope, full clocks), it runs the job the PC left in job.env (tools/deck_test.py), writes
# job.state and exits, and Steam returns to the library. Without job.env it plays the game normally.
#
# job.env: KIND=sweep|run|boot|checks  ID=<job id>  LEVELS=<sweep levels>  SECONDS=<limit>  MENU_SCALE=<x>  ENV=<extra env>
cd "$(dirname "$(readlink -f "$0")")"
JOB=job.env
if [ ! -f "$JOB" ]; then exec ./country-box; fi
KIND=; ID=; LEVELS=; SECONDS_=480; MENU_SCALE=1.0; ENV=; SWEEP_ARGS=
# shellcheck disable=SC1090
. "./$JOB"
ID=${ID:-$(date +%Y%m%d-%H%M%S)}
OUT=results/$ID
mkdir -p "$OUT"
mv "$JOB" "$OUT/job.env"   # consumed: a later launch from the library plays the game
echo "running $KIND $(date +%s)" > job.state
{
  uname -a; cat /etc/os-release; ls -l /dev/udmabuf
  cat /sys/class/power_supply/BAT1/status /sys/class/power_supply/BAT1/capacity 2>/dev/null
} > "$OUT/system.txt" 2>&1
rc=0
case "$KIND" in
  checks)
    ./platform_check > "$OUT/platform_check.log" 2>&1 || rc=$?
    ./memory_check > "$OUT/memory_check.log" 2>&1 || rc=$?
    ./userptr_wp_check > "$OUT/userptr_wp_check.log" 2>&1 || rc=$?
    ./country-box --probe-kernel > "$OUT/probe-kernel.log" 2>&1 || rc=$?
    ./country-box --boot > "$OUT/boot.log" 2>&1 || rc=$? ;;
  boot)
    ./country-box --boot > "$OUT/boot.log" 2>&1 || rc=$? ;;
  run)   # the title and menus for SECONDS_, with frame times and shots
    env $ENV MP4_SHOTS=w0-${SECONDS_}:10 MP4_SHOTS_DIR="$OUT" ./country-box --seconds "$SECONDS_" > "$OUT/run.log" 2>&1 || rc=$? ;;
  sweep)
    env $ENV python3 sweep.py --build . --out "$OUT" ${LEVELS:+--levels "$LEVELS"} --seconds "$SECONDS_" \
        --menu-scale "$MENU_SCALE" $SWEEP_ARGS > "$OUT/sweep.out" 2>&1 || rc=$? ;;
  *) echo "unknown KIND=$KIND" > "$OUT/error.txt"; rc=2 ;;
esac
echo "done $KIND $rc $(date +%s) $OUT" > job.state
exit 0
