# Level sweep — Returns HD

A tool for loading any level directly and measuring the whole game without playing it by hand. It serves two purposes: finding what fails to load or looks wrong, and measuring in **milliseconds** what to expect on the Steam Deck (frame time and CPU per thread).

## Parts

| Part | What it does |
|---|---|
| `core/runtime/dkcr_tools.c` | Warp, level table, launch mode and per-level measurement (`CB_*` variables) |
| `core/runtime/rt_base.c` `rt_entry_hook_set` | Hooks at the entry of lifted functions (`FN_ENTER`); the hook can return through the function |
| `core/tools/sweep.py` | Runs one process per level and writes `report.md` / `report.csv` outside the repo |

## How the game starts a level (*measured* on the code of `main`, v0)

The island map stores the target world's name hash in the game state; the scene loader then reads it, looks up the level and loads it after DK's barrel transition. The warp rewrites those two world ids at the entry of the scene loader, so the rest of the loading is the game's own path. The game's level table has 83 entries (`CB_WARP=list` prints it).

## Super Guide: the game plays itself

The game's Super Guide restart mode makes the level start with Super Kong, which runs through it using the game's own recording. `CB_LAUNCH_MODE=superguide` changes the mode the map launches the level with (also `timeattack` = 6 and `replay` = 7). *Measured* on 1-3: Super Kong finishes the level in ~120 s and the game shows LEVEL CLEAR.

## Measurement (per level, in ms)

- **Start**: first frame of 30 in a row in which the game clock advances ; during loading it does not advance (*measured*). Loading = from the warp to that frame.
- **End**: the LEVEL CLEAR / WORLD CLEAR summary screen (hooked), the state moves to another world (a boss may return to the map without a summary), or the `--seconds` limit. Measurement stops at LEVEL CLEAR; the capture `end.png` (`CB_END_SHOT`) shows the final screen. The game clock is **not** a valid end: it stops during the Mugly fight (*measured*), and during cutscenes.
- **Frame**: interval between presentations; mean, p50, p95, p99, max., frames > 16.9 and > 33.4 ms.
- **CPU**: ms of CPU per frame for each thread (`/proc/self/task`, utime+stime) and their sum. The game's threads are named `g<handle> p<priority>`; `guest-main` is the `nnMain` thread.
- **Replication / GPU / waits**: from the `MP4_TRACE=perf` lines inside the measured window.

Hooks have a per-address filter: `FN_ENTER` reads one byte of `g_fnhook` (index = constant address of the function) and only calls the host where there is a hook or a break. Without the filter, an active hook cost every lifted call (*measured*: 8.2 → 9.5 ms of CPU per frame on 1-3).

## Usage

```sh
# one level by hand (from the build folder; the save must have Game 1)
CB_WARP=l03_jungle_rambi CB_LAUNCH_MODE=superguide MP4_INPUT_SCRIPT=menus.txt ./country-box --seconds 400

# sweep
core/tools/sweep.py --build <build dir> --out <output dir> \
    [--levels 12,14,minecart] [--mode superguide|normal|timeattack] [--seconds 480]
```

Variables: `CB_WARP=<WorldID|part of the name|list>`, `CB_LAUNCH_MODE`, `CB_WARP_EXIT=0` (continue after the level), `CB_WARP_TRACE=1` (game clock every second), `CB_INVINCIBLE=1` (no effect seen: the retail build does not read `CheatsData`), `CB_ALLOW_PTRACE=1` (allows `eu-stack -p` to sample threads).

### Keeping a long run alive

A process launched from a session that ends later (e.g. a remote or managed shell, even with `setsid nohup`) can receive SIGTERM when that session goes away: SDL turns it into "window closed" and the level is lost (*measured* on 2026-11-11, twice). A long sweep should run as a user transient unit, outside the launching session's process group (no root needed; it disappears when it finishes):

```sh
systemd-run --user --collect --unit=cb-sweep --working-directory=<your copy>/country-box/core \
    --setenv=WAYLAND_DISPLAY=$WAYLAND_DISPLAY --setenv=DISPLAY=$DISPLAY --setenv=XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
    -p StandardOutput=append:<out>/sweep.out -p StandardError=append:<out>/sweep.out \
    /usr/bin/python3 tools/sweep.py --build <build dir> --out <out> --resume
systemctl --user status cb-sweep        # follow progress; --resume picks up from the last level with a result
```

`sweep.py` kills and records a level whose log stops growing for `--stall` s (180), and samples the sensors every second (`telemetry.csv` per level; summary in the sensor table of `report.md`).

## On the Steam Deck

`tools/deck_bundle.sh` puts `sweep.py`, `sweep.sh` and the template save (`profile/`, Game 1) into the bundle. On the Deck: `./run.sh` (checks, boot, title screen), then `./sweep.sh [levels] [menu scale]`. If the Deck reaches the title later than the PC, the menu buttons are scaled (`--menu-scale 1.5`). The Deck's `amdgpu` sensor (Van Gogh) reads the power of the whole APU (*assumed*; to be confirmed with the battery unplugged: `bat_w`).

## Finding from first use

Using `eu-stack` on the busiest thread: the guest's `SleepThread` with waits ≤ 60 µs was spinning in `kernel.c sleep_ns`. A Returns HD worker thread polls with short waits non-stop: 16.4 ms of CPU out of every 16.7. Now the guest's sleep never spins (`plat_sleep_lax_ns`): CPU for level 1-3 went from 24 → 8.5 ms per frame, with the same frame times (mean 16.67, p99 ≈ 18 ms).
