#!/usr/bin/env python3
# Country-Box: level sweep for Donkey Kong Country Returns HD and Tropical Freeze. Runs the built game once per level:
# the scripted menus reach the first level, the warp replaces it with the level under test (Returns: runtime/dkcr_tools.c
# CB_WARP=<WorldID>; Tropical Freeze: runtime/ov_dktf.c CB_TF_WARP=<area name>), by default in Super Guide mode (Super
# Kong plays the level from the game's own recording), and the run ends when the level is done or at the time limit.
# Both games print the same "level: ..." lines, so one parser reads both. Each run's log is parsed into one report row, in milliseconds:
#   frame time mean/p50/p95/p99/max, CPU ms per frame (all threads and the busiest ones), render-thread replay ms,
#   GPU ms, load time, pipeline/shader waits, crashes; plus screenshots; and the machine's own sensors while the level
#   is measured (Telemetry: amdgpu power, clock and temperature, CPU temperature and clock, battery draw), which on a
#   Steam Deck give the APU's power in watts.
# The save the menus start from is copied from a template profile for every run, so runs do not change each other.
#
#   tools/sweep.py --build <build dir> --out <report dir> [--levels 12,14,rambi,...] [--mode superguide|normal|timeattack]
#                  [--game auto|dkcrhd|dktf] [--headless 1920x1080] [--movies]
#
# Movies are skipped (CB_SKIP_MOVIES=1, runtime/ov_movie.c) unless --movies; the environment's own value wins.
# --game: auto reads the program id of the build's exefs/main.npdm (the path modules.c names). --headless runs each
# level inside gamescope's headless back end (no window on the user's screen, no focus taken) at that output size.
# Tropical Freeze's levels come from the game itself: the first sweep runs it once with CB_TF_WARP=list and keeps the
# area list in <build>/tf-areas.txt (one "index name" per line); levels are the areas whose names --tf-levels matches.
#
# Game-derived output (logs, shots, report) goes to --out, outside the repository.
import argparse
import csv
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time


# Kind of each level, from its name: what the sweep reports as the special types.
def kind(name):
    if 'boss' in name: return 'jefe'
    # the game's level-name suffixes: w00_trophy_..._<world>_k are the K levels, ..._temple_9_9 the Golden Temple's last level,
    # l0x_<world>_<name>_9_<n> the Golden Temple levels kept with their world's assets, factory_launch is 7-R.
    if 'trophy_temple' in name: return 'templo dorado (9-9)'
    if 'trophy_cloudintro' in name: return 'intro (nube)'
    if 'trophy' in name: return 'nivel K'
    if name.startswith('l0x_'): return 'templo dorado'
    if 'minecart' in name or 'mctutorial' in name or 'clingcarts' in name: return 'vagoneta'
    if 'rocketbarrel' in name or 'launch' in name: return 'barril cohete'
    return 'normal'

# The menus from the title to 1-1's PLAY on the island map, for a save that has Game 1 (the build's profile).
MENU_SCRIPT = """# sweep: title -> Game 1 -> island map -> Jungle -> 1-1 PLAY (the warp replaces the level)
@25000 L 300
@25000 R 300
@30000 A 200
@33000 A 200
@36000 A 200
@39000 A 200
@42000 A 200
@45000 A 200
@50000 A 200
@55000 A 200
@62000 A 200
@68000 A 200
"""

# Tropical Freeze: from the title, new game with one player in the Original mode (the PLAY MODE screen offers Funky
# Mode first), to the first level (the warp replaces it). Screens seen with shots (skipped
# movies): title 10-12 s, GAME SELECT 14-16, PLAY MODE 18-20, CONTROLLER SELECT 22-24, single player 26-28.
TF_MENU_SCRIPT = """# sweep (Tropical Freeze): title -> GAME SELECT new game -> PLAY MODE: Original (right from Funky) -> controller,
# single player, character -> first level (the warp replaces it). The As after the menus end the skipped movies
# (CB_SKIP_MOVIES: the first input ends one); the warp drops what is left of the script.
@12000 L 400
@12000 R 400
@16000 A 200
@18500 RIGHT 200
@20000 A 200
@24000 A 200
@28000 A 200
@32000 A 200
@36000 A 200
@40000 A 200
@44000 A 200
@48000 A 200
@52000 A 200
@56000 A 200
@60000 A 200
@64000 A 200
@68000 A 200
@72000 A 200
@76000 A 200
@80000 A 200
@84000 A 200
@88000 A 200
@92000 A 200
@96000 A 200
@100000 A 200
@104000 A 200
@108000 A 200
@112000 A 200
@116000 A 200
@120000 A 200
"""

# Program ids (exefs/main.npdm ACI0 +0x10), measured on the retail dumps (runtime/settings.c has the same pair).
GAME_IDS = {0x01009D901BC56000: 'dkcrhd', 0x0100C1F0051B6000: 'dktf'}
GAME_TITLES = {'dkcrhd': 'Returns HD', 'dktf': 'Tropical Freeze'}


def build_game(build):
    """The game a build runs: the program id of the exefs its modules.c names (rt_default_exefs), else None."""
    text = _read(os.path.join(build, 'modules.c')) or ''
    m = re.search(r'rt_default_exefs\[\] = "([^"]*)"', text)
    try:
        with open(os.path.join(m.group(1), 'main.npdm'), 'rb') as f:
            d = f.read()
        aci = int.from_bytes(d[0x70:0x74], 'little')
        if d[:4] != b'META' or d[aci:aci + 4] != b'ACI0':
            return None
        return GAME_IDS.get(int.from_bytes(d[aci + 0x10:aci + 0x18], 'little'))
    except (AttributeError, OSError, ValueError):
        return None


def tf_kind(name):
    """Kind of a Tropical Freeze area, from its name (CB_TF_WARP=list, 1.0.0: 63 areas, all levels): b00_<world>_* the
    world's boss, l0a_ / l0b_ the world's two extra levels, *_secret_* the secret world, *_trophy_* the K levels."""
    n = name.lower()
    if n.startswith('b00_'): return 'jefe'
    if '_trophy_' in n: return 'nivel K'
    if '_secret_' in n: return 'mundo secreto'
    if n.startswith(('l0a_', 'l0b_')): return 'nivel extra'
    if 'minecart' in n: return 'vagoneta'
    if 'rocketbarrel' in n: return 'barril cohete'
    return 'normal'


def returns_levels(a, wrap):
    """Returns HD's levels as (WorldID, name): <build>/levels.txt, made by running the game once with CB_WARP=list
    (dkcr_tools.c logs the game's own level table at the first map launch). Nothing of the table ships in the repo:
    it comes from the player's copy. WorldID 9 (load screen) and 10 (map) are not levels."""
    path = os.path.join(a.build, 'levels.txt')
    if not os.path.exists(path):
        log = os.path.join(a.out, 'levels.log')
        prof = tempfile.mkdtemp(prefix='cb-sweep-profile-')
        shutil.copytree(os.path.join(a.build, 'profile'), prof, dirs_exist_ok=True)
        env = dict(os.environ, CB_WARP='list', MP4_INPUT_SCRIPT=os.path.join(a.out, 'menus.txt'))
        with open(log, 'w') as f:
            p = subprocess.Popen(wrap([os.path.join(a.build, 'country-box'), '--profile', prof, '--seconds', '120']),
                                 cwd=a.build, env=env, stdout=f, stderr=subprocess.STDOUT)
            t0 = time.time()
            while p.poll() is None and time.time() - t0 < 600:
                time.sleep(2)
                if "warp: the game's level table" in (_read(log) or ''):   # one burst of lines
                    time.sleep(3)
                    break
            if p.poll() is None:
                p.kill()
                p.wait()
        shutil.rmtree(prof, ignore_errors=True)
        rows = re.findall(r'warp: +(-?\d+) +[0-9a-f]{8} +(\S+)', _read(log) or '')
        levels = [(int(w), n) for w, n in rows if int(w) not in (9, 10)]
        if not levels:
            sys.exit(f'sweep: no level table in {log} (CB_WARP=list printed nothing)')
        with open(path, 'w') as f:
            f.writelines(f'{w} {n}\n' for w, n in levels)
    with open(path) as f:
        return [(int(w), n) for w, n in (line.split() for line in f if line.strip())]


def tf_levels(a, wrap):
    """Tropical Freeze's areas as (index, name): <build>/tf-areas.txt, made by running the game once with
    CB_TF_WARP=list (ov_dktf.c logs "tfwarp: area <index> <guid> <name>" when it first reads the universe)."""
    path = os.path.join(a.build, 'tf-areas.txt')
    if not os.path.exists(path):
        log = os.path.join(a.out, 'tf-areas.log')
        prof = tempfile.mkdtemp(prefix='cb-sweep-profile-')
        env = dict(os.environ, CB_TF_WARP='list', MP4_INPUT_SCRIPT=os.path.join(a.out, 'menus.txt'))
        if not a.movies:
            env.setdefault('CB_SKIP_MOVIES', '1')
        with open(log, 'w') as f:
            p = subprocess.Popen(wrap([os.path.join(a.build, 'country-box'), '--profile', prof, '--seconds', '120']),
                                 cwd=a.build, env=env, stdout=f, stderr=subprocess.STDOUT)
            t0 = time.time()
            while p.poll() is None and time.time() - t0 < 600:
                time.sleep(2)
                if 'tfwarp: area' in (_read(log) or ''):   # the whole list is one burst of lines
                    time.sleep(3)
                    break
            if p.poll() is None:
                p.kill()
                p.wait()
        shutil.rmtree(prof, ignore_errors=True)
        areas = re.findall(r'tfwarp: area +(\d+) [0-9a-f]{32} (\S+)', _read(log) or '')
        if not areas:
            sys.exit(f'sweep: no area list in {log} (CB_TF_WARP=list printed nothing)')
        with open(path, 'w') as f:
            f.writelines(f'{i} {n}\n' for i, n in areas)
    out = []
    for line in open(path):
        i, _, n = line.strip().partition(' ')
        if n and re.search(a.tf_levels, n):
            out.append((int(i), n))
    return out


def headless_wrap(size):
    """The command line that runs a game command under gamescope's headless back end at size (WxH): the game sees an
    X11 display of that size and the desktop never shows it. None: run it as it is."""
    if not size:
        return lambda cmd: cmd
    w, h = size.lower().split('x')
    return lambda cmd: (['gamescope', '--backend', 'headless', '-W', w, '-H', h, '-w', w, '-h', h, '-r', '60', '--',
                         'env', '-u', 'WAYLAND_DISPLAY', 'SDL_VIDEO_DRIVER=x11', 'MP4_ASSUME_FOCUS=1'] + cmd)


FLOAT = r'([0-9.]+)'


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


class Telemetry:
    """Sensor samples once a second (sysfs, no root): every amdgpu hwmon's power, edge temperature and shader clock,
    k10temp's Tctl, the average CPU clock and a battery's power and status. On a Steam Deck (Van Gogh) the amdgpu
    power is the APU's socket power, CPU and GPU together; on a desktop it is the graphics card's."""

    def __init__(self):
        self.gpus, self.cpu_temp = [], None
        for h in sorted(glob.glob('/sys/class/hwmon/hwmon*')):
            name = _read(os.path.join(h, 'name'))
            if name == 'amdgpu':
                pw = next((os.path.join(h, f) for f in ('power1_average', 'power1_input') if os.path.exists(os.path.join(h, f))), None)
                self.gpus.append((h, pw))
            elif name == 'k10temp':
                self.cpu_temp = os.path.join(h, 'temp1_input')
        self.cpu_freqs = glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq')
        self.bat = next(iter(sorted(glob.glob('/sys/class/power_supply/BAT*'))), None)

    def sample(self):
        r = {}
        for i, (h, pw) in enumerate(self.gpus):
            p, t, f = _read(pw) if pw else None, _read(os.path.join(h, 'temp1_input')), _read(os.path.join(h, 'freq1_input'))
            if p: r[f'gpu{i}_w'] = int(p) / 1e6
            if t: r[f'gpu{i}_c'] = int(t) / 1000
            if f: r[f'gpu{i}_mhz'] = int(f) / 1e6
        t = _read(self.cpu_temp) if self.cpu_temp else None
        if t: r['cpu_c'] = int(t) / 1000
        fr = [int(x) for x in (_read(p) for p in self.cpu_freqs) if x]
        if fr: r['cpu_mhz'] = sum(fr) / len(fr) / 1000
        if self.bat:
            pn, st = _read(os.path.join(self.bat, 'power_now')), _read(os.path.join(self.bat, 'status'))
            if pn is None:
                c, v = _read(os.path.join(self.bat, 'current_now')), _read(os.path.join(self.bat, 'voltage_now'))
                pn = str(int(c) * int(v) // 1000000) if c and v else None
            if pn: r['bat_w'] = int(pn) / 1e6
            if st: r['bat_status'] = st
        return r


def vram_used_mib():
    """VRAM in use on the first amdgpu device (all processes), or None."""
    for d in sorted(glob.glob('/sys/class/drm/card*/device')):
        v = _read(os.path.join(d, 'mem_info_vram_used'))
        if v: return int(v) >> 20
    return None


def wait_for_vram(limit_mib, log):
    """Before a level: wait while other processes hold more than limit_mib of VRAM (a local model loaded on the GPU
    left the game 0.75 GB of budget instead of 13.9 and doubled its slow frames, 2026-11-11). Returns the MiB in use."""
    t0, said = time.time(), False
    while True:
        u = vram_used_mib()
        if u is None or u <= limit_mib:
            return u
        if not said:
            print(f'sweep: {u} MiB of VRAM in use by other programs (limit {limit_mib}); waiting', flush=True)
            said = True
        if time.time() - t0 > 3600:
            print('sweep: VRAM still busy after an hour; running anyway (the report marks it)', flush=True)
            return u
        time.sleep(30)


def summarize(samples):
    """Mean and max of each numeric sensor over the measured samples."""
    out = {}
    keys = sorted({k for s in samples for k, v in s.items() if isinstance(v, (int, float))})
    for k in keys:
        v = [s[k] for s in samples if k in s]
        out[k] = (round(sum(v) / len(v), 1), round(max(v), 1))
    st = [s['bat_status'] for s in samples if 'bat_status' in s]
    if st: out['bat_status'] = max(set(st), key=st.count)
    return out


def parse(log):
    """One report row from a run's log."""
    r = {}
    with open(log, errors='replace') as f:
        text = f.read()
    m = re.search(r'level: (\S+) playing ' + FLOAT + r' s after the warp', text)
    r['cargado'] = bool(m)
    m = re.search(r'level: result (\S+) WorldID (-?\d+) end (.+?), ' + FLOAT + r' s and (\d+) frames measured, load ' + FLOAT, text)
    if m:
        r['fin'] = m.group(3)
        r['medido_s'] = float(m.group(4))
        r['frames'] = int(m.group(5))
        r['carga_s'] = float(m.group(6))
    m = re.search(r'level: frame ms mean ' + FLOAT + ' p50 ' + FLOAT + ' p95 ' + FLOAT + ' p99 ' + FLOAT + ' max ' + FLOAT +
                  r'; over 16.9 ms (\d+), over 33.4 ms (\d+)', text)
    if m:
        for k, v in zip(('ms_media', 'ms_p50', 'ms_p95', 'ms_p99', 'ms_max'), m.groups()[:5]):
            r[k] = float(v)
        r['sobre_16_9'] = int(m.group(6))
        r['sobre_33_4'] = int(m.group(7))
    m = re.search(r'level: dropped frames with control (\d+) \(in (\d+) frames with control, worst ' + FLOAT +
                  r' ms\), without control (\d+)', text)
    if m:
        r['caidas_con_control'] = int(m.group(1))
        r['frames_con_control'] = int(m.group(2))
        r['peor_con_control_ms'] = float(m.group(3))
        r['caidas_sin_control'] = int(m.group(4))
    m = re.search(r'level: cpu ms per frame ' + FLOAT + r' in all threads; busiest: (.*)', text)
    if m:
        r['cpu_ms_total'] = float(m.group(1))
        r['cpu_hilos'] = m.group(2).strip()
    # MP4_TRACE=perf summaries inside the measured window (the frames after the level's "measuring" line)
    start = text.find('; measuring')
    window = text[start:] if start >= 0 else ''
    rep = [float(x) for x in re.findall(r'replay avg ' + FLOAT + ' ms', window)]
    gpu = [float(x) for x in re.findall(r'gpu avg ' + FLOAT + ' ms', window)]
    if rep: r['replay_ms'] = round(sum(rep) / len(rep), 2)
    if gpu: r['gpu_ms'] = round(sum(gpu) / len(gpu), 2)
    waits = re.findall(r'one-shot waited (\d+)', window)
    r['espera_pipeline'] = sum(int(x) for x in re.findall(r'waited (\d+), waiting for a shader', window))
    r['espera_shader'] = sum(int(x) for x in re.findall(r'waiting for a shader (\d+)', window))
    r['espera_unica'] = sum(int(x) for x in waits)
    budgets = [int(x) for x in re.findall(r'vram \d+ of (\d+) MiB', window)]
    if budgets: r['vram_presupuesto_min'] = min(budgets)
    fatal = re.search(r'(FATAL[^\n]*|\[host\] fatal:[^\n]*)', text)
    r['error'] = fatal.group(1)[:160] if fatal else ''
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', required=True, help='build dir with country-box and its profile/')
    ap.add_argument('--out', required=True, help='report dir (outside the repository)')
    ap.add_argument('--levels', default='', help='WorldIDs or name parts, comma-separated (default: all)')
    ap.add_argument('--mode', default='superguide', choices=('superguide', 'normal', 'timeattack'))
    ap.add_argument('--seconds', type=int, default=480, help='time limit of a run (world clock, from the first present)')
    ap.add_argument('--no-perf-trace', action='store_true', help='without MP4_TRACE=perf (no replay/GPU ms)')
    ap.add_argument('--shots', type=int, default=20, help='seconds between screenshots (0: none)')
    ap.add_argument('--resume', action='store_true', help='skip levels whose run.log already has a result')
    ap.add_argument('--report-only', action='store_true', help='rebuild the report from the existing run.log files')
    ap.add_argument('--stall', type=int, default=180, help='kill a run whose log stops growing for this many s (a hang)')
    ap.add_argument('--vram-limit', type=int, default=5000,
                    help='MiB of VRAM other programs may hold before a level starts; above it the sweep waits')
    ap.add_argument('--menu-scale', type=float, default=1.0,
                    help='multiply the menu presses\' times (a slower machine reaches the title later: 1.5 on a Deck that does)')
    ap.add_argument('--game', default='auto', choices=('auto', 'dkcrhd', 'dktf'),
                    help='the game the build runs (auto: from its exefs)')
    ap.add_argument('--headless', default='', metavar='WxH',
                    help='run each level in gamescope\'s headless back end at this size (no window, e.g. 1920x1080)')
    ap.add_argument('--hz', type=int, default=60,
                    help='fast-forward: CB_TURBO_HZ (the menus and the load at 60, the level at N from its first measured '
                         'present: the games step a fixed 1/60 s per frame, so 240 plays about 4x faster when the CPU '
                         'keeps up; at N from boot the scripted menus break); for coverage and stress only: frame ms are '
                         'not valid then')
    ap.add_argument('--note', default='', help='a line under the report\'s title (what kind of run it is)')
    ap.add_argument('--movies', action='store_true',
                    help='play the game\'s movies (by default CB_SKIP_MOVIES=1 ends each at once: no opening every run)')
    ap.add_argument('--tf-levels', default=r'.', metavar='REGEX',
                    help='Tropical Freeze: the areas (names in <build>/tf-areas.txt) that are levels')
    a = ap.parse_args()

    game = a.game if a.game != 'auto' else (build_game(a.build) or 'dkcrhd')
    a.title = GAME_TITLES[game]
    wrap = headless_wrap(a.headless)
    os.makedirs(a.out, exist_ok=True)
    script = os.path.join(a.out, 'menus.txt')
    with open(script, 'w') as f:   # @<ms> times scaled for a slower boot
        f.write(re.sub(r'^@(\d+)', lambda m: '@%d' % round(int(m.group(1)) * a.menu_scale),
                       TF_MENU_SCRIPT if game == 'dktf' else MENU_SCRIPT, flags=re.M))
    levels, kind_of = (tf_levels(a, wrap), tf_kind) if game == 'dktf' else (returns_levels(a, wrap), kind)
    want = levels
    if a.levels:
        keys = [k.strip() for k in a.levels.split(',') if k.strip()]
        want = [lv for lv in levels if any((k.isdigit() and int(k) == lv[0]) or (not k.isdigit() and k in lv[1]) for k in keys)]
    template = os.path.join(a.build, 'profile')
    if game == 'dktf' and not os.path.isdir(template):
        template = None   # a new game from the title: no save needed
    elif not os.path.isdir(template):
        sys.exit(f'sweep: no profile/ in {a.build} (play the menus once to make a Game 1 save)')
    tele = Telemetry()
    rows = []
    for wid, name in want:
        d = os.path.join(a.out, f'{wid:02d}-{name}')
        log = os.path.join(d, 'run.log')
        done = os.path.exists(log) and 'level: result' in open(log, errors='replace').read()
        if a.report_only or (a.resume and done):
            if os.path.exists(log):
                r = parse(log)
                r.update(worldid=wid, nivel=name, tipo=kind_of(name), salida='', reloj_s='')
                fast_forward(a, r)
                r.update(sensors(summarize(read_telemetry(os.path.join(d, 'telemetry.csv')))))
                rows.append(r)
            continue
        os.makedirs(d, exist_ok=True)
        vram_before = wait_for_vram(a.vram_limit, log)
        prof = tempfile.mkdtemp(prefix='cb-sweep-profile-')
        if template:
            shutil.copytree(template, prof, dirs_exist_ok=True)
        env = dict(os.environ, MP4_INPUT_SCRIPT=script)
        env.update({'CB_TF_WARP': name} if game == 'dktf' else {'CB_WARP': str(wid)})
        if not a.movies:   # ov_movie.c: each movie ends at once (the openings would replay in every run)
            env.setdefault('CB_SKIP_MOVIES', '1')
        if a.hz != 60:     # menus and loading at 60, then vsync events and pacing at N once the level plays
            env['CB_TURBO_HZ'] = str(a.hz)
        if a.mode != 'normal':
            env['CB_LAUNCH_MODE'] = a.mode
        if not a.no_perf_trace:
            env['MP4_TRACE'] = 'perf'
        if a.shots:
            env.update(MP4_SHOTS=f'w75-{a.seconds}:{a.shots}', MP4_SHOTS_DIR=d)
        if env.get('CB_SPIKE_DIAG') == '1':   # a spike's frame, to tell gameplay from a loading screen
            env['CB_SPIKE_SHOTS'] = d
        t0 = time.time()
        hung = False
        with open(log, 'w') as f:
            env['CB_END_SHOT'] = os.path.join(d, 'end.png')   # not --shot: that makes pipeline builds synchronous
            p = subprocess.Popen(wrap([os.path.join(a.build, 'country-box'), '--profile', prof, '--seconds', str(a.seconds)]),
                                 cwd=a.build, env=env, stdout=f, stderr=subprocess.STDOUT)
            size, still = -1, time.time()
            phase, pos, measured = 'load', 0, []
            with open(os.path.join(d, 'telemetry.csv'), 'w', newline='') as tf:
                tw = None
                while p.poll() is None:   # a run whose log stops growing (no present, no log line) is hung
                    time.sleep(1)
                    with open(log, errors='replace') as lf:   # the measured window, from the log's level lines
                        lf.seek(pos)
                        new = lf.read()
                        pos = lf.tell()
                    if phase == 'load' and '; measuring' in new:
                        phase = 'measure'
                    if phase == 'measure' and 'level: result' in new:
                        phase = 'after'
                    smp = dict(tele.sample(), t=round(time.time() - t0, 1), phase=phase)
                    if phase == 'measure':
                        measured.append(smp)
                    if tw is None:
                        tw = csv.DictWriter(tf, fieldnames=list(smp), extrasaction='ignore')
                        tw.writeheader()
                    tw.writerow(smp)
                    n = os.path.getsize(log)
                    if n != size:
                        size, still = n, time.time()
                    elif time.time() - still > a.stall or time.time() - t0 > a.seconds + 600:
                        hung = True
                        p.kill()
                        p.wait()
        shutil.rmtree(prof, ignore_errors=True)
        r = parse(log)
        if hung and not r['error']:
            r['error'] = f'colgado: el log no creció en {a.stall} s'
        r.update(worldid=wid, nivel=name, tipo=kind_of(name), salida=p.returncode, reloj_s=round(time.time() - t0))
        fast_forward(a, r)
        r.update(sensors(summarize(measured)))
        r['vram_otros_mib'] = vram_before
        rows.append(r)
        print(f"{wid:2d} {name:32s} exit {p.returncode} {r.get('fin', 'no medido'):24s} "
              f"ms media {r.get('ms_media', '-')} p99 {r.get('ms_p99', '-')} cpu {r.get('cpu_ms_total', '-')} "
              f"{r['error']}", flush=True)
        write_report(a, rows)
    write_report(a, rows)


# The frame-time columns, which a fast-forward run (--hz) does not measure: the game is paced at another rate.
FRAME_COLS = ('ms_media', 'ms_p50', 'ms_p95', 'ms_p99', 'ms_max', 'sobre_16_9', 'sobre_33_4', 'caidas_con_control',
              'frames_con_control', 'peor_con_control_ms', 'caidas_sin_control', 'replay_ms', 'gpu_ms')


def fast_forward(a, r):
    """A fast-forward run's row: marked, its frame times dropped (only completion, crashes, CPU and wall time count)."""
    if a.hz == 60:
        return
    r['modo'] = f'fast-forward {a.hz} Hz'
    for k in FRAME_COLS:
        r.pop(k, None)


def read_telemetry(path):
    """The measured samples of a telemetry.csv written by an earlier run."""
    if not os.path.exists(path):
        return []
    out = []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            if row.get('phase') != 'measure':
                continue
            s = {}
            for k, v in row.items():
                try:
                    s[k] = float(v)
                except (TypeError, ValueError):
                    s[k] = v
            out.append(s)
    return out


def sensors(m):
    """Report columns from summarize(): the first amdgpu device (the Deck's APU; on the desktop the dGPU unless an
    iGPU is listed first) and the CPU and battery."""
    g = lambda k, i=0: m[k][i] if k in m else ''
    return {'gpu_w': g('gpu0_w'), 'gpu_w_max': g('gpu0_w', 1), 'gpu_c_max': g('gpu0_c', 1), 'gpu_mhz': g('gpu0_mhz'),
            'cpu_c_max': g('cpu_c', 1), 'cpu_mhz': g('cpu_mhz'), 'bat_w': g('bat_w'), 'bat_status': m.get('bat_status', '')}


def write_report(a, rows):
    cols = ['worldid', 'nivel', 'tipo', 'salida', 'cargado', 'fin', 'carga_s', 'medido_s', 'frames', 'ms_media', 'ms_p50',
            'ms_p95', 'ms_p99', 'ms_max', 'sobre_16_9', 'sobre_33_4', 'caidas_con_control', 'frames_con_control',
            'peor_con_control_ms', 'caidas_sin_control', 'cpu_ms_total', 'replay_ms', 'gpu_ms',
            'espera_pipeline', 'espera_shader', 'espera_unica', 'gpu_w', 'gpu_w_max', 'gpu_c_max', 'gpu_mhz', 'cpu_c_max',
            'cpu_mhz', 'bat_w', 'bat_status', 'vram_presupuesto_min', 'vram_otros_mib', 'cpu_hilos', 'error', 'reloj_s']
    if a.hz != 60:
        cols.append('modo')
    with open(os.path.join(a.out, 'report.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, '') for c in cols})
    with open(os.path.join(a.out, 'report.md'), 'w') as f:
        f.write(f'# Barrido de niveles — {a.title} (modo {a.mode})\n\n')
        if a.note:
            f.write(f'**{a.note}**\n\n')
        if a.hz != 60:
            f.write(f'**Avance rápido a {a.hz} Hz (`--hz`, `CB_TURBO_HZ`: menús y carga a 60, el nivel a {a.hz} desde que se '
                    f'juega)**: el juego avanza 1/60 s por frame, así que va unas {a.hz / 60:.1f} veces más rápido; los ms por frame no son válidos y no se muestran. Cuentan sólo si el nivel terminó, '
                    'los cierres, la CPU y el tiempo de reloj.\n\n')
        f.write('Tiempos en ms por frame, medidos desde que la escena del nivel terminó de cargar hasta que el nivel '
                'terminó (o el límite de tiempo). CPU: suma de todos los hilos por frame; réplica: trabajo del hilo de '
                'render; GPU: tiempo de GPU por frame.\n\n')
        f.write('| ID | nivel | tipo | fin | carga s | media | p50 | p95 | p99 | máx | >33 ms | caídas con control (peor ms) | '
                'caídas sin control | CPU | réplica | GPU | esperas pipeline/shader | error |\n'
                '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n')
        for r in rows:
            g = lambda k: r.get(k, '–')
            f.write(f"| {g('worldid')} | {g('nivel')} | {g('tipo')} | {g('fin')} | {g('carga_s')} | {g('ms_media')} | "
                    f"{g('ms_p50')} | {g('ms_p95')} | {g('ms_p99')} | {g('ms_max')} | {g('sobre_33_4')} | "
                    f"{g('caidas_con_control')} ({g('peor_con_control_ms')}) | {g('caidas_sin_control')} | "
                    f"{g('cpu_ms_total')} | {g('replay_ms')} | {g('gpu_ms')} | "
                    f"{g('espera_pipeline')}/{g('espera_shader')} | {r.get('error') or ''} |\n")
        if a.hz != 60:
            f.write(f'\nAvance rápido a {a.hz} Hz: tiempo de reloj por nivel (lanzamiento a salida).\n\n'
                    '| ID | nivel | fin | salida | reloj s | CPU | error |\n|---|---|---|---|---|---|---|\n')
            for r in rows:
                g = lambda k: r.get(k, '–')
                f.write(f"| {g('worldid')} | {g('nivel')} | {g('fin')} | {g('salida')} | {g('reloj_s')} | {g('cpu_ms_total')} | "
                        f"{r.get('error') or ''} |\n")
        f.write('\nSensores durante la medición (media; máx.). amdgpu W: en la Deck es la APU entera (CPU+GPU); en la PC, la '
                'tarjeta gráfica. Batería: potencia que entrega o recibe, con su estado.\n\n'
                '| ID | nivel | amdgpu W | W máx | GPU °C máx | GPU MHz | CPU °C máx | CPU MHz | batería W |\n'
                '|---|---|---|---|---|---|---|---|---|\n')
        for r in rows:
            g = lambda k: r.get(k, '') if r.get(k, '') != '' else '–'
            bat = f"{g('bat_w')} ({r.get('bat_status')})" if r.get('bat_status') else g('bat_w')
            f.write(f"| {g('worldid')} | {g('nivel')} | {g('gpu_w')} | {g('gpu_w_max')} | {g('gpu_c_max')} | {g('gpu_mhz')} | "
                    f"{g('cpu_c_max')} | {g('cpu_mhz')} | {bat} |\n")
        f.write('\nHilos más ocupados (ms de CPU por frame):\n\n')
        for r in rows:
            f.write(f"- {r.get('worldid')} {r.get('nivel')}: {r.get('cpu_hilos', '–')}\n")


if __name__ == '__main__':
    main()
