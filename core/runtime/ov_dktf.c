// Country-Box: SDK overrides Donkey Kong Country: Tropical Freeze (1.0.0, SDK 4.4.0, Retro's GameFramework) needs, in
// the role ov_dkcr.c has for Returns HD: the SDK function is replaced by symbol (host.h HOST_OVERRIDE), so the service
// behind it is never opened. Each entry names the call that reached it on the start-up path and what the game does
// with the answer. The network overrides of ov_dkcr.c (ssl, socket, nifm: offline) serve this game as well.
#include <math.h>
#include <unistd.h>
#include "input_map.h"
#include "host.h"
#include "level_run.h"
#include "ov.h"

// ==== nn::pctl
// bool CheckFreeCommunicationPermission(bool show_ui): CGameState's constructor (main+0x333f7c) asks whether parental
// controls allow free communication with other users. A console without parental controls answers true; the game
// stays offline either way (nifm never has a connection, ov_dkcr.c).
static void ov_pctl_free_communication(Cpu *c) { ret_x(c, 1); }
HOST_OVERRIDE("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", ov_pctl_free_communication);

// ==== nn::oe
// int GetUserInactivityDetectionTimeExtensionMode(): CScreenDimmingUtil::DisableScreenDimming (main+0x64824, on the way
// into a level) asks it and, when it reads 0, extends the time with SetUserInactivityDetectionTimeExtended(true)
// (ov_system.c, a no-op: the host never dims the screen). The lifted SDK read an applet object nn::oe::Initialize
// never made (ov_system.c takes Initialize over). 0: not extended.
static void ov_oe_inactivity_mode(Cpu *c) { ret_x(c, 0); }
HOST_OVERRIDE("_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv", ov_oe_inactivity_mode);

// ==== CB_TF_TRACE=<symbol or module+0xoffset>[,...] (at most 24): a log line at the 1st, 2nd, 4th, 8th, ... entry of
// each listed lifted function, with the seconds since start, to see which steps of a state machine run and which never
// do (the 1-1 intro that stays on its first shot). The bodies run unchanged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"
#define TF_TRACE_MAX 24
static struct { char name[96]; uint64_t addr; volatile long hits; } tf_trace[TF_TRACE_MAX];
static uint64_t tf_t0;

static int tf_trace_hook(Cpu *c, void *arg) {
    int i = (int)(intptr_t)arg;
    long n = __atomic_add_fetch(&tf_trace[i].hits, 1, __ATOMIC_RELAXED);
    if (!(n & (n - 1)))
        rt_log("tftrace: %s #%ld at %.2f s (x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx)", tf_trace[i].name, n,
               (double)(plat_now_ns() - tf_t0) / 1e9, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4]);
    return 0;
}

// CB_TF_CAMPROBE=1: every 30th CCinematicCamera::CCinematicCameraBehavior::Update (main+0x323bf8, 1.0.0) logs the
// behaviour, its timer (this+0x18, counted down by dt), dt (s0) and the camera state's done byte (x3+0x61), which ends
// a shot whose timer is not positive.
static int tf_cam_probe(Cpu *c, void *arg) {
    (void)arg;
    static volatile long n;
    long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k % 30 == 1) {
        float timer, dt;
        uint64_t self = c->x[0];
        memcpy(&timer, (const void *)(uintptr_t)(self + 0x18), 4);
        memcpy(&dt, &c->v[0], 4);
        uint8_t done = *(const uint8_t *)(uintptr_t)(c->x[3] + 0x61);
        rt_log("tfcam: #%ld at %.2f s behaviour %#llx camera %#llx timer %.3f dt %.4f state %#llx done %u", k,
               (double)(plat_now_ns() - tf_t0) / 1e9, (unsigned long long)self,
               (unsigned long long)*(const uint64_t *)(uintptr_t)(self + 0x10), timer, dt, (unsigned long long)c->x[3], done);
    }
    return 0;
}

// CAnimatedCameraBehavior::Update (main+0x5c32fc): the animated camera's CCharacter (this+0x50; NULL: the shot can never
// end) and its done flag (this+0x38).
static int tf_anim_cam_probe(Cpu *c, void *arg) {
    (void)arg;
    static volatile long n;
    long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k % 30 == 1) {
        uint64_t self = c->x[0];
        rt_log("tfcam: animated #%ld at %.2f s behaviour %#llx character %#llx flag38 %u", k,
               (double)(plat_now_ns() - tf_t0) / 1e9, (unsigned long long)self,
               (unsigned long long)*(const uint64_t *)(uintptr_t)(self + 0x50), *(const uint8_t *)(uintptr_t)(self + 0x38));
    }
    return 0;
}

// CB_TF_MSGLOG=<seconds>: from that many seconds after start, the first 400 script messages handled
// (CStateManagerObject::HandleScriptMsg, main+0x2cfcd4, 1.0.0), each as its sender and target ids (+0, +4: CScriptMsg::Create
// stores (action, sender, target, event) and HandleScriptMsg dispatches to ObjectById([msg+4])), action (+0x40) and
// event (+0x44) as FourCC, and the class of both ends: the object list at CStateManagerObject+0xf0068 holds 16-byte
// slots indexed by the id's low 16 bits, the slot's first word the component whose +8 is the id and whose first word
// the vtable (CGameObjectList::ObjectById, main+0x2d4648). CB_TF_VTABLES=<file> names the vtables: one "0x<main
// offset> <class>" per line (from main's _ZTV symbols; the file stays outside the repo), else the main offset is shown.
static double tf_msg_from;
static struct { uint64_t off; char *name; } *tf_vt;
static int tf_vt_n;
static void tf_vt_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("tfmsg: cannot read %s", path); return; }
    char line[256];
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        char *end;
        uint64_t off = strtoull(line, &end, 16);
        while (*end == ' ') end++;
        end[strcspn(end, "\r\n")] = 0;
        if (!*end) continue;
        if (tf_vt_n == cap) tf_vt = realloc(tf_vt, (size_t)(cap = cap ? cap * 2 : 1024) * sizeof *tf_vt);
        tf_vt[tf_vt_n].off = off;
        tf_vt[tf_vt_n].name = strdup(end);
        tf_vt_n++;
    }
    fclose(f);
    rt_log("tfmsg: %d vtables from %s", tf_vt_n, path);
}
static void tf_msg_fourcc(uint32_t v, char out[5]) {
    for (int i = 0; i < 4; i++) {
        unsigned char ch = (unsigned char)(v >> (8 * (3 - i)));
        out[i] = ch >= 0x20 && ch < 0x7f ? (char)ch : '.';
    }
    out[4] = 0;
}
static const char *tf_msg_class(uint64_t sm, uint32_t id, char *buf, int n) {
    if (id == 0xffffffffu) return "-";
    uint64_t slot = sm + 0xf0068 + (uint64_t)(id & 0xffff) * 16, comp;
    memcpy(&comp, (const void *)(uintptr_t)slot, 8);
    uint32_t have = 0;
    if (comp) memcpy(&have, (const void *)(uintptr_t)(comp + 8), 4);
    if (!comp || have != id) return "?";
    uint64_t vt;
    memcpy(&vt, (const void *)(uintptr_t)comp, 8);
    uint64_t main_base = 0;
    for (int k = 0; k < ldr_count(); k++)
        if (!strcmp(ldr_module(k)->name, "main")) main_base = ldr_module(k)->base;
    uint64_t off = vt - main_base - 0x10;   // the object points 16 bytes into the vtable symbol (past offset/RTTI)
    for (int i = 0; i < tf_vt_n; i++)
        if (tf_vt[i].off == off) return tf_vt[i].name;
    snprintf(buf, n, "main+%#llx", (unsigned long long)(vt - main_base));
    return buf;
}
static int tf_msg_probe(Cpu *c, void *arg) {
    (void)arg;
    double t = (double)(plat_now_ns() - tf_t0) / 1e9;
    if (t < tf_msg_from) return 0;
    static volatile long n;
    long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k <= 400) {
        const uint32_t *m = (const uint32_t *)(uintptr_t)c->x[2];
        char a[5], e[5], sb[160], tb[160];
        tf_msg_fourcc(m[16], a);
        tf_msg_fourcc(m[17], e);
        const char *sc = tf_msg_class(c->x[0], m[0], sb, sizeof sb), *tc = tf_msg_class(c->x[0], m[1], tb, sizeof tb);
        rt_log("tfmsg: %.2f s %08x %s -> %08x %s action %s event %s", t, m[0], sc, m[1], tc, a, e);
    }
    return 0;
}


// CB_TF_TIMERPROBE=<seconds>: from that second on, every 60th CTimerGOC::Think (main+0x463474: id this+8, time left
// this+0x88, running this+0x84, active bits this+0xc) and CTimerSequenceGOC::Think (main+0x464628: id, elapsed this+0xf0,
// running this+0xf4 (0: Think returns), paused this+0x96) with dt (s0): whether the timers started by a cinematic shot tick at all and how
// far they are from firing (CTimerGOC fires IE00 when its time left reaches 0).
static double tf_timer_from;
static int tf_timer_probe(Cpu *c, void *arg) {
    double t = (double)(plat_now_ns() - tf_t0) / 1e9;
    if (t < tf_timer_from) return 0;
    static volatile long n;
    long k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k % 60 != 1 || k > 60 * 400) return 0;
    const uint8_t *o = (const uint8_t *)(uintptr_t)c->x[0];
    uint32_t id;
    float dt, f;
    memcpy(&id, o + 8, 4);
    memcpy(&dt, &c->v[0], 4);
    if (arg == NULL) {
        memcpy(&f, o + 0x88, 4);
        rt_log("tftimer: %.2f s CTimerGOC %08x left %.3f running %u active %#x dt %.4f", t, id, f, o[0x84], o[0xc], dt);
    } else {
        memcpy(&f, o + 0xf0, 4);
        rt_log("tftimer: %.2f s CTimerSequenceGOC %08x elapsed %.3f running %u paused %u active %#x dt %.4f", t, id, f, o[0xf4],
               o[0x96], o[0xc], dt);
        // Country-Box (tf): NewTime (main+0x464648): length +0x8c, loop flag +0x95 with period +0x90, tracks: count +0x78,
        // array +0x80 of 0x10-byte records (u16 +0, event count +8, events +0x10: 0x10 each, time float at +0).
        float len, period;
        int32_t ntracks;
        uint64_t tracks;
        memcpy(&len, o + 0x8c, 4), memcpy(&period, o + 0x90, 4), memcpy(&ntracks, o + 0x78, 4), memcpy(&tracks, o + 0x80, 8);
        static uint32_t dumped[16];
        int seen = 0;
        for (int i = 0; i < 16; i++) seen |= dumped[i] == id;
        if (!seen) {
            for (int i = 0; i < 16; i++) if (!dumped[i]) { dumped[i] = id; break; }
            rt_log("tftimer:   %08x length %.3f loop %u period %.3f tracks %d at %#llx", id, len, o[0x95], period, ntracks,
                   (unsigned long long)tracks);
            for (int i = 0; tracks && i < ntracks && i < 8; i++) {
                const uint8_t *tr = (const uint8_t *)(uintptr_t)(tracks + (uint64_t)i * 0x18);
                uint16_t tag;
                int32_t nev;
                uint64_t ev;
                memcpy(&tag, tr, 2), memcpy(&nev, tr + 8, 4), memcpy(&ev, tr + 0x10, 8);
                char line[256];
                int w = snprintf(line, sizeof line, "tftimer:     track %d tag %u events %d:", i, tag, nev);
                for (int j = 0; ev && j < nev && j < 8 && w < (int)sizeof line - 16; j++) {
                    float et;
                    memcpy(&et, (const void *)(uintptr_t)(ev + (uint64_t)j * 0x10), 4);
                    w += snprintf(line + w, sizeof line - (size_t)w, " %.3f", et);
                }
                rt_log("%s", line);
            }
        }
    }
    return 0;
}

// ==== CB_TF_WARP=<name part>|list: the level the game is sent to first is replaced by the named one (Tropical Freeze
// 1.0.0, measured statically). CUniverseInfo (main+0x280d40 GetAreaInfo: int count at +0, then count CAreaInfo of 0x50
// bytes from +8) lists every area: CAreaInfo +0 its GUID (CObjectId, 16 bytes), +0x10 its name (rstl string, char* at
// +0). CGameState::SetDesiredWorldId (main+0x335f4c) copies the 16 bytes at x1 into the game state (+0x14): the hook
// rewrites them before the copy, once. "list" only logs the areas (with their index) the first time the universe is
// read.
static const char *tf_warp;
static volatile uint64_t tf_universe, tf_state;   // CUniverseInfo, CGameState (x0 of SetDesiredWorldId)
static uint8_t tf_target[16];                      // the warped area's GUID
static char tf_target_name[96], tf_left[160];

// CGameState +0xc8: total play time in seconds (double; CGameState::SetTotalPlayTimeInSeconds main+0x336204), advanced
// by CStateManagerGameData::UpdateLevelPlayTime only while a level is played (not loading, no cinematic camera).
static double tf_play_time(void) {
    double t = 0;
    if (tf_state) memcpy(&t, (const void *)(uintptr_t)(tf_state + 0xc8), 8);
    return t;
}
static volatile int32_t tf_warp_done, tf_listed;

static const char *tf_area_name(uint64_t area) {
    uint64_t p = *(const uint64_t *)(uintptr_t)(area + 0x10);
    return p ? (const char *)(uintptr_t)p : "";
}

static int tf_universe_hook(Cpu *c, void *arg) {
    (void)arg;
    uint64_t u = c->x[0];
    if (!u) return 0;
    tf_universe = u;
    if (plat_atomic_cas(&tf_listed, 1, 0) == 0) {
        int32_t n = *(const int32_t *)(uintptr_t)u;
        rt_log("tfwarp: universe %#llx, %d areas", (unsigned long long)u, n);
        for (int i = 0; i < n && i < 400; i++) {
            uint64_t a = u + 8 + (uint64_t)i * 0x50;
            const uint32_t *g = (const uint32_t *)(uintptr_t)a;
            rt_log("tfwarp: area %3d %08x%08x%08x%08x %s", i, g[0], g[1], g[2], g[3], tf_area_name(a));
        }
    }
    return 0;
}

static int tf_desired_world_hook(Cpu *c, void *arg) {
    (void)arg;
    uint8_t *id = (uint8_t *)(uintptr_t)c->x[1];
    if (c->x[0]) tf_state = c->x[0];
    uint64_t u = tf_universe;
    const char *want = tf_warp;
    if (!id || !u || !want || !strcmp(want, "list") || tf_warp_done) {
        if (id) {
            const uint32_t *g = (const uint32_t *)id;
            rt_log("tfwarp: SetDesiredWorldId %08x%08x%08x%08x", g[0], g[1], g[2], g[3]);
        }
        return 0;
    }
    int32_t n = *(const int32_t *)(uintptr_t)u;
    for (int i = 0; i < n; i++) {
        uint64_t a = u + 8 + (uint64_t)i * 0x50;
        if (strstr(tf_area_name(a), want)) {
            if (plat_atomic_cas(&tf_warp_done, 1, 0)) return 0;
            rt_log("tfwarp: the game asked for area %08x...; sent to area %d %s instead", *(const uint32_t *)id, i,
                   tf_area_name(a));
            int from = -1;
            for (int k = 0; k < n; k++)
                if (!memcmp((const void *)(uintptr_t)(u + 8 + (uint64_t)k * 0x50), id, 16)) from = k;
            // Returns HD's line (tools/sweep.py): the WorldID is the area's index in CUniverseInfo, the hash its GUID's
            // first word.
            rt_log("warp: the map launched %s (WorldID %d); loading %s (WorldID %d, hash %08x) instead",
                   from >= 0 ? tf_area_name(u + 8 + (uint64_t)from * 0x50) : "?", from, tf_area_name(a), i,
                   *(const uint32_t *)(uintptr_t)a);
            memcpy(id, (const void *)(uintptr_t)a, 16);
            memcpy(tf_target, id, 16);
            snprintf(tf_target_name, sizeof tf_target_name, "%s", tf_area_name(a));
            // CB_WARP_KEEP_SCRIPT=1: the script goes on into the level (scripted menu tests: pause, Options)
            const char *keep = getenv("CB_WARP_KEEP_SCRIPT");
            if (!keep || !*keep || !strcmp(keep, "0")) input_script_end();
            level_run_warped(tf_target_name, i, tf_play_time());
            return 0;
        }
    }
    rt_log("tfwarp: no area named like '%s' (CB_TF_WARP=list shows them)", want);
    plat_atomic_xchg(&tf_warp_done, 1);
    return 0;
}

// The run's end when the game leaves the warped area: "left for <area>".
static const char *tf_left_for(void) {
    uint64_t u = tf_universe;
    snprintf(tf_left, sizeof tf_left, "another world");
    if (u && tf_state) {
        int32_t n = *(const int32_t *)(uintptr_t)u;
        for (int k = 0; k < n; k++)
            if (!memcmp((const void *)(uintptr_t)(u + 8 + (uint64_t)k * 0x50), (const void *)(uintptr_t)(tf_state + 0x14), 16))
                snprintf(tf_left, sizeof tf_left, "left for %s", tf_area_name(u + 8 + (uint64_t)k * 0x50));
    }
    return tf_left;
}

// CHUDIOWin::ShowEOLResults (main+0x33d66c; CEndLevelGOC::OnAction_ShowResults tail-calls it): the level-clear screen,
// the counterpart of Returns HD's LevelSummaryUIController.Activate.
static int tf_clear_hook(Cpu *c, void *arg) {
    (void)c, (void)arg;
    if (level_run_active()) level_run_clear("CHUDIOWin::ShowEOLResults");
    return 0;
}

// CB_SKIP_MOVIES=1 for Tropical Freeze: its player ends a movie by time, not by end of stream: CMoviePlayer::Update
// (main+0x5e8398) stops when ContinuePlaying (main+0x5e86fc) finds the seconds played (this+0xc) at the movie's total
// (this+8). ov_movie.c's skip (end of stream at the first input) left it running the movie's full length on a black
// screen (1-1's Demo1.mp4, run-70). At ContinuePlaying's entry the played time is set to the total, so the movie ends
// this frame through the game's own end path (data contexts deactivated, frame cleared).
static int tf_movie_skip_hook(Cpu *c, void *arg) {
    (void)arg;
    float *p = (float *)(uintptr_t)c->x[0];
    if (p && p[2] > 0 && p[3] < p[2]) {
        rt_log("tfmovie: skipped at %.1f of %.1f s (CB_SKIP_MOVIES)", p[3], p[2]);
        p[3] = p[2];
    }
    return 0;
}

// CB_TF_BOT (below dktf_tools_init)
static int tf_bot_on;
static int tf_prethink_hook(Cpu *c, void *arg);
static int tf_invulnerable_hook(Cpu *c, void *arg);
static int tf_no_damage_hook(Cpu *c, void *arg);
static void tf_bot_input(uint64_t *b, int32_t ls[2]);

// The byte is set at the entry of NGameModeSetup::setup_primary_gamemode, not at start: the game's constructors (DT_INIT,
// after this file's init) reset the production flags.
static uint64_t tf_main;
static int tf_superguide_hook(Cpu *c, void *arg) {
    (void)c, (void)arg;
    // The function reads the flag through its GOT slot (main+0x1a23708), so the byte written is the one it points to.
    uint64_t flag = *(const uint64_t *)(uintptr_t)(tf_main + 0x1a23708);
    if (flag) *(volatile uint8_t *)(uintptr_t)flag = 1;
    static int said;
    if (!said++)
        rt_log("tfwarp: setup_primary_gamemode with gAutoSuperGuide = 1 at %#llx (main+%#llx; Super Kong where the level has "
               "a recording)", (unsigned long long)flag, (unsigned long long)(flag - tf_main));
    return 0;
}

#define TF_PROGRAM_ID 0x0100C1F0051B6000ull

void dktf_tools_init(void) {
    tf_t0 = plat_now_ns();
    // Every hook below is at a Tropical Freeze 1.0.0 address: another game (Returns HD shares CB_LAUNCH_MODE and
    // CB_SKIP_MOVIES) gets none of them.
    if (rt_program_id() != TF_PROGRAM_ID || rt_game_version != 100) return;
    for (int k = 0; k < ldr_count(); k++)   // the PC settings in the game's Options screen (dktf_options.c)
        if (!strcmp(ldr_module(k)->name, "main")) dktf_options_init(ldr_module(k)->base);
    for (int k = 0; k < ldr_count(); k++)   // MP4_ASPECT=native: the panel's shape (dktf_aspect.c)
        if (!strcmp(ldr_module(k)->name, "main")) dktf_aspect_init(ldr_module(k)->base);
    // CB_LAUNCH_MODE=superguide (as for Returns HD): RProductionFlags::gAutoSuperGuide (main+0x5e505dc) set, so
    // NGameModeSetup::setup_primary_gamemode (main+0x1e6d48) builds CSuperGuideGameMode for a level that has a recording
    // (Super Kong plays it) and falls back to the normal mode otherwise.
    const char *lm = getenv("CB_LAUNCH_MODE");
    if (lm && *lm) {
        if (strcmp(lm, "superguide")) rt_fatal("CB_LAUNCH_MODE=%s: Tropical Freeze knows superguide only", lm);
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) {
                tf_main = ldr_module(k)->base;
                rt_entry_hook_set(tf_main + 0x1e6d48, tf_superguide_hook, NULL);
            }
        rt_log("tfwarp: CB_LAUNCH_MODE=superguide armed (gAutoSuperGuide set at each game-mode setup)");
    }
    const char *sk = getenv("CB_SKIP_MOVIES");
    if (sk && !strcmp(sk, "1"))
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) rt_entry_hook_set(ldr_module(k)->base + 0x5e86fc, tf_movie_skip_hook, NULL);
    const char *bt = getenv("CB_TF_BOT"), *inv = getenv("CB_TF_INVINCIBLE");
    tf_bot_on = bt && !strcmp(bt, "1");
    if (tf_bot_on || (inv && !strcmp(inv, "1")))
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) {
                uint64_t m = ldr_module(k)->base;
                rt_entry_hook_set(m + 0x2055e0, tf_invulnerable_hook, NULL);
                rt_entry_hook_set(m + 0x1fa714, tf_no_damage_hook, NULL);
                rt_entry_hook_set(m + 0x1fe3e8, tf_no_damage_hook, NULL);
                if (tf_bot_on) {
                    rt_entry_hook_set(m + 0x1f88a8, tf_prethink_hook, NULL);
                    g_input_bot = tf_bot_input;
                }
            }
    if (tf_bot_on) rt_log("tfbot: on (invincible, hold right, jump/hover, unstick)");
    const char *wp = getenv("CB_TF_WARP");
    if (wp && *wp) {
        tf_warp = wp;
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) {
                rt_entry_hook_set(ldr_module(k)->base + 0x280d40, tf_universe_hook, NULL);
                rt_entry_hook_set(ldr_module(k)->base + 0x335f4c, tf_desired_world_hook, NULL);
                rt_entry_hook_set(ldr_module(k)->base + 0x33d66c, tf_clear_hook, NULL);
            }
        rt_log("tfwarp: CB_TF_WARP=%s armed", wp);
    }
    const char *ml = getenv("CB_TF_MSGLOG");
    if (ml && *ml) {
        tf_msg_from = atof(ml);
        if (getenv("CB_TF_VTABLES")) tf_vt_load(getenv("CB_TF_VTABLES"));
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) rt_entry_hook_set(ldr_module(k)->base + 0x2cfcd4, tf_msg_probe, NULL);
    }
    const char *tp = getenv("CB_TF_TIMERPROBE");
    if (tp && *tp) {
        tf_timer_from = atof(tp);
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) {
                rt_entry_hook_set(ldr_module(k)->base + 0x463474, tf_timer_probe, NULL);
                rt_entry_hook_set(ldr_module(k)->base + 0x464628, tf_timer_probe, (void *)1);
            }
    }
    const char *cp = getenv("CB_TF_CAMPROBE");
    if (cp && !strcmp(cp, "1"))
        for (int k = 0; k < ldr_count(); k++)
            if (!strcmp(ldr_module(k)->name, "main")) {
                rt_entry_hook_set(ldr_module(k)->base + 0x323bf8, tf_cam_probe, NULL);
                rt_entry_hook_set(ldr_module(k)->base + 0x5c32fc, tf_anim_cam_probe, NULL);
            }
    const char *e = getenv("CB_TF_TRACE");
    if (!e || !*e) return;
    int n = 0;
    for (const char *p = e; *p && n < TF_TRACE_MAX;) {
        size_t len = strcspn(p, ",");
        char s[96];
        snprintf(s, sizeof s, "%.*s", (int)len, p);
        uint64_t a = 0;
        const char *plus = strchr(s, '+');
        if (plus && !strncmp(s, "main+", 5)) {
            for (int k = 0; k < ldr_count(); k++)
                if (!strcmp(ldr_module(k)->name, "main")) a = ldr_module(k)->base + strtoull(plus + 1, NULL, 16);
        } else
            a = ldr_symbol(s);
        if (a) {
            snprintf(tf_trace[n].name, sizeof tf_trace[n].name, "%s", s);
            tf_trace[n].addr = a;
            rt_entry_hook_set(a, tf_trace_hook, (void *)(intptr_t)n);
            n++;
        } else
            rt_log("tftrace: %s not found", s);
        p += len + (p[len] == ',');
    }
    rt_log("tftrace: %d functions traced", n);
}

// ==== CB_TF_MEASURE=<from>,<to>: frame times between those seconds after start (the presenting thread's interval
// between presents) and each thread's CPU over the window (/proc/self/task/*/stat utime + stime), reported once as
// "tfmeasure:" lines in the same terms as Returns HD's level report (dkcr_tools.c): ms mean / p50 / p95 / p99 / max,
// frames over 16.9 and 33.4 ms, CPU ms per frame in all threads and the busiest ones. With CB_SPIKE_DIAG=1 each
// present of the window also goes to rt_spike_diag (spike_diag.c; TF's player control is not known: -1).
#include <dirent.h>
#define TF_TASKS 128
typedef struct { char name[24]; uint64_t ticks; int tid; } TfTask;
static int tf_tasks(TfTask *t, int max) {
    DIR *d = opendir("/proc/self/task");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], buf[512];
        snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;
        char *l = strchr(buf, '('), *r = strrchr(buf, ')');
        if (!l || !r) continue;
        snprintf(t[n].name, sizeof t[n].name, "%.*s", (int)(r - l - 1), l + 1);
        unsigned long long ut = 0, st = 0;
        // fields after ')': state(3) ... utime(14) stime(15)
        sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
        t[n].ticks = ut + st;
        t[n].tid = atoi(e->d_name);
        n++;
    }
    if (d) closedir(d);
    return n;
}
static int tf_cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

static void tf_measure_frame(uint64_t frame);


// ==== CB_TF_BOT=1: a traversal bot for sweeps (Tropical Freeze has no Super Guide data). It holds right and jumps
// (A held: Funky Mode hovers), and when the player stops advancing it jumps longer, rolls (Y) and backs off. With
// CB_TF_INVINCIBLE=1 (the bot sets it too) the players take no damage: CPlayer::IsInvulnerable (main+0x2055e0) answers
// true, CPlayer::RemovePlayerHitPoints (0x1fa714) and CPlayer::ApplyKillDamage (0x1fe3e8, pits and crushes) do nothing.
// Progress: the first player's translation ([[CPlayer+0x100]+0x80]+0x30, CPlayer::GetTranslation 0x1fabb4), logged as
// "tfbot:" lines every 5 s and at the end of the run. All offsets: 1.0.0.
static volatile uint64_t tf_player;
static double tf_bot_last_pt = -1;
static uint64_t tf_bot_pt_ns;      // when the play time last advanced
static float tf_bot_maxx, tf_bot_x0;
static int tf_bot_have;

static int tf_pos(float *x, float *y) {
    uint64_t p = tf_player;
    if (!p) return 0;
    uint64_t a = *(const uint64_t *)(uintptr_t)(p + 0x100);
    if (!a) return 0;
    uint64_t t = *(const uint64_t *)(uintptr_t)(a + 0x80);
    if (!t) return 0;
    memcpy(x, (const void *)(uintptr_t)(t + 0x30), 4);
    memcpy(y, (const void *)(uintptr_t)(t + 0x34), 4);
    return 1;
}

static int tf_prethink_hook(Cpu *c, void *arg) {
    (void)arg;
    if (!tf_player && c->x[0]) tf_player = c->x[0];
    return 0;
}
static int tf_invulnerable_hook(Cpu *c, void *arg) { (void)arg; ret_x(c, 1); return 1; }
static int tf_no_damage_hook(Cpu *c, void *arg) { (void)c, (void)arg; return 1; }

// Polling thread (input_sdl.c), every input read.
static void tf_bot_input(uint64_t *b, int32_t ls[2]) {
    uint64_t now = plat_now_ns();
    if (!tf_bot_pt_ns || now - tf_bot_pt_ns > 500000000ull) return;   // not playing (loading, cinematic, menus)
    static uint64_t t_start, t_progress, t_unstick;
    static float best;
    float x, y;
    if (!tf_pos(&x, &y)) return;
    if (!t_start) t_start = t_progress = now, best = x;
    if (x > best + 0.3f) best = x, t_progress = now;
    double since = (double)(now - t_start) / 1e9, stuck = (double)(now - t_progress) / 1e9;
    int right = 1, jump = fmod(since, 1.0) < 0.5, roll = 0;
    if (stuck > 1.5) {   // not advancing: longer jumps, a roll now and then
        jump = fmod(since, 1.4) < 0.9;
        roll = fmod(since, 2.0) < 0.1;
    }
    if (stuck > 6.0) {   // still stuck: back off for a moment, then come again
        double k = fmod(stuck - 6.0, 4.0);
        if (k < 0.8) right = 0, ls[0] = -32767;
    }
    if (stuck > 30.0 && !t_unstick) t_unstick = now;
    if (right) ls[0] = 32767;
    if (jump) *b |= 1ull << B_A;
    if (roll) *b |= 1ull << B_Y;
}

static void tf_bot_frame(void) {
    double pt = tf_play_time();
    uint64_t now = plat_now_ns();
    if (pt != tf_bot_last_pt) tf_bot_last_pt = pt, tf_bot_pt_ns = now;
    float x, y;
    if (!tf_pos(&x, &y)) return;
    if (!tf_bot_have) tf_bot_x0 = tf_bot_maxx = x, tf_bot_have = 1;
    if (x > tf_bot_maxx) tf_bot_maxx = x;
    static uint64_t last;
    if (now - last >= 5000000000ull) {
        last = now;
        rt_log("tfbot: x %.1f y %.1f, furthest %.1f (%.1f from the start)", x, y, tf_bot_maxx, tf_bot_maxx - tf_bot_x0);
    }
}

// Every present: the warped level's run (level_run.c) while one is on, else CB_TF_MEASURE. 1: the run ended.
int dktf_tools_frame(uint64_t frame) {
    if (tf_bot_on) tf_bot_frame();
    if (tf_state && level_run_active())
        return level_run_frame(frame, tf_play_time(), !memcmp((const void *)(uintptr_t)(tf_state + 0x14), tf_target, 16), -1,
                               tf_left_for);
    tf_measure_frame(frame);
    return 0;
}

static void tf_measure_frame(uint64_t frame) {
    static int on = -1;
    static double from, to;
    static uint64_t t_last;
    static float *iv;
    static int niv, cap, done;
    static TfTask t0[TF_TASKS];
    static int nt0;
    if (on < 0) {
        const char *e = getenv("CB_TF_MEASURE");
        on = e && sscanf(e, "%lf,%lf", &from, &to) == 2 && to > from;
    }
    if (!on || done) return;
    uint64_t now = plat_now_ns();
    double t = (double)(now - tf_t0) / 1e9;
    if (t < from) { t_last = now; return; }
    if (!nt0) nt0 = tf_tasks(t0, TF_TASKS), t_last = now;
    else {
        if (niv == cap) iv = realloc(iv, (size_t)(cap = cap ? cap * 2 : 8192) * sizeof *iv);
        iv[niv++] = (float)((now - t_last) / 1e6);
        if (rt_spike_diag_on()) rt_spike_diag(frame, (now - t_last) / 1e6, -1);   // CB_SPIKE_DIAG (spike_diag.c)
        t_last = now;
    }
    if (t < to || niv < 2) return;
    done = 1;
    double sum = 0;
    int over16 = 0, over33 = 0;
    for (int i = 0; i < niv; i++) sum += iv[i], over16 += iv[i] > 16.9f, over33 += iv[i] > 33.4f;
    float *s = malloc((size_t)niv * sizeof *s);
    memcpy(s, iv, (size_t)niv * sizeof *s);
    qsort(s, (size_t)niv, sizeof *s, tf_cmpf);
    rt_log("tfmeasure: %.1f s to %.1f s, %d frames (frame %llu): ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f; over 16.9 ms %d, "
           "over 33.4 ms %d", from, t, niv, (unsigned long long)frame, sum / niv, s[niv / 2], s[(int)(niv * 0.95)],
           s[(int)(niv * 0.99)], s[niv - 1], over16, over33);
    free(s);
    static TfTask t1[TF_TASKS];
    int nt1 = tf_tasks(t1, TF_TASKS);
    double hz = (double)sysconf(_SC_CLK_TCK), total = 0;
    struct { char name[24]; double ms; } busy[TF_TASKS];
    int nb = 0;
    for (int i = 0; i < nt1; i++) {
        uint64_t before = 0;
        for (int j = 0; j < nt0; j++) if (t0[j].tid == t1[i].tid) before = t0[j].ticks;
        double ms = (double)(t1[i].ticks - before) * 1000.0 / hz / niv;
        total += ms;
        int k = 0;
        for (; k < nb && strcmp(busy[k].name, t1[i].name); k++) {}
        if (k == nb) snprintf(busy[nb].name, sizeof busy[nb].name, "%s", t1[i].name), busy[nb++].ms = 0;
        busy[k].ms += ms;
    }
    for (int i = 0; i < nb; i++)
        for (int j = i + 1; j < nb; j++)
            if (busy[j].ms > busy[i].ms) { __typeof__(busy[0]) x = busy[i]; busy[i] = busy[j]; busy[j] = x; }
    char line[512];
    int w = snprintf(line, sizeof line, "tfmeasure: cpu ms per frame %.2f in all threads; busiest:", total);
    for (int i = 0; i < nb && i < 10 && w < (int)sizeof line - 40; i++)
        w += snprintf(line + w, sizeof line - (size_t)w, "%s %s %.2f", i ? "," : "", busy[i].name, busy[i].ms);
    rt_log("%s", line);
}
