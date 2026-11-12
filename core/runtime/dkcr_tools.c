// Country-Box: test tools for Donkey Kong Country Returns HD (1.0.0), for the level sweep (tools/sweep.py): a level
// warp, the game's level list, and an invincible player. Off unless their variables are set; each works through entry
// hooks (rt_base.c rt_entry_hook_set) on the game's own IL2CPP methods and removes them once done, so a measured run
// pays nothing for them after the warp.
//
//   CB_WARP=<WorldID>|<name part>   the first level the island map launches is replaced by this one
//   CB_WARP=list                    logs the game's level table (WorldID, name, name hash) at the first map launch
//   CB_INVINCIBLE=1                 the player cannot be killed (the game's own CheatsData.PlayerCantBeKilled; the
//                                   release build may never read it: no effect seen on 1-3's spikes)
//   CB_WARP_EXIT=0                  keep running after the warped level ends (by default the run ends there)
//   CB_LAUNCH_MODE=superguide|timeattack|replay   the map launches the level in that mode (ERestartMode 5, 6, 7):
//                                   superguide = Super Kong plays the level from the game's recording
//
// How the game starts a level (measured from main's IL2CPP code, symbols from Cpp2IL; offsets are main+, version 0):
// the island map's CMapManager.LaunchLevel (main+0x26fd260) stores the chosen node's world name hash into the game
// state (gpGameState.mDesiredWorldId +0x60, mCurrentWorldId +0x64), then SceneLoader.LoadSceneTieWipe (main+0x1f7fcc0)
// reads mDesiredWorldId, finds its LevelInfo by name hash (Globals.LevelInfoFindByNameHash) and loads that scene
// behind the DK barrel transition. The warp rewrites both ids at LoadSceneTieWipe's entry, so the rest of the load is
// the game's own path. The level table is Main.Instance.m_CommonData.Levels (List<LevelInfo>).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "level_run.h"
#include "platform/platform.h"

#define OFF_LOAD_SCENE_TIE_WIPE 0x1f7fcc0ull   // SceneLoader.LoadSceneTieWipe(ETransitionType)
#define OFF_MAP_LAUNCH_LEVEL 0x26fd260ull      // CMapManager.LaunchLevel(IScriptObject areaNodeId, ERestartMode)
// Summary.LevelSummaryUIController.Activate: the LEVEL CLEAR / WORLD CLEAR screen. Not LevelSummaryController.
// StartLevelSummary, which the Super Guide ending never calls (MP4_BREAK_AT on 1-3: Activate once, from main+0x2979ae0).
#define OFF_START_LEVEL_SUMMARY 0x1d4c220ull
#define OFF_CANT_BE_KILLED 0x1d887e0ull        // UnityToolbox.CheatsData.get_PlayerCantBeKilled()
// The two metadata slots are read from the lifted code (the ADRP pages it computes): Cpp2IL's ISIL dump prints an
// ADRP's immediate without the instruction's page added (0x3424000 in LevelInfoFindByNameHash is main+0x5388000).
#define OFF_GLOBALS_CLASS 0x53878e8ull         // metadata slot: Il2CppClass* of dk3ds.Globals (CGameStateSetDesiredWorldId)
#define OFF_MAIN_CLASS 0x5388c38ull            // metadata slot: Il2CppClass* of dk3ds.Main (LevelInfoFindByNameHash)
#define CLASS_STATICS 0xb8                     // Il2CppClass.static_fields
#define CLASS_CCTOR_DONE 0xe0                  // Il2CppClass.cctor_finished
#define GLOBALS_GAME_STATE 0xa48               // Globals.gpGameState (static)
#define GLOBALS_INPUT_ENABLED 0x3ec            // Globals static bool read by get_IsInputEnabled (main+0x1f0bf50)
#define GLOBALS_WORLD 0xfe8                    // Globals static object whose +0x58 WorldIsPaused (main+0x1f64d40) reads
#define WORLD_PAUSED 0x58
#define GAME_STATE_DESIRED 0x60                // GameStateProfileData.mDesiredWorldId
#define GAME_STATE_CURRENT 0x64                // GameStateProfileData.mCurrentWorldId
#define GAME_STATE_PLAY_TIME 0x4c              // GameStateProfileData.mTotalPlayTime (float, seconds)
#define MAIN_INSTANCE 0x8                      // Main.<Instance>k__BackingField (static)
#define MAIN_COMMON_DATA 0x50                  // Main.m_CommonData
#define COMMON_LEVELS 0x18                     // CommonDataAsset.Levels
#define LIST_ITEMS 0x10                        // List<T>._items
#define LIST_SIZE 0x18                         // List<T>._size
#define ARRAY_DATA 0x20                        // T[] elements
#define LEVEL_NAME 0x18                        // LevelInfo.name (string)
#define LEVEL_NAME_HASH 0x20                   // LevelInfo.name_hash
#define LEVEL_WORLD_ID 0x58                    // LevelInfo.SceneReference.WorldID
#define STRING_LENGTH 0x10                     // System.String length, UTF-16 chars from +0x14
#define CHEATS_CANT_BE_KILLED 0x18             // CheatsData.m_playerCantBeKilled
#define WORLD_FRONTEND 9
#define WORLD_ISLANDMAP 10

static uint64_t g_main;   // main's load address
static const char *g_warp;

// The warped level's run (level_run.c): dkcr_tools_frame gives it the game state's play time (mTotalPlayTime, which
// stands still while a level loads: 1-3, measured), whether the game is still in the warped level, and whether the
// player has control (Globals input enabled, world not paused); LevelSummaryUIController.Activate is its LEVEL CLEAR.
static uint64_t g_state;                  // gpGameState at the warp
static uint32_t g_target;                 // the level's name hash
static uint64_t rdp(uint64_t a);
static uint32_t rd32(uint64_t a);
static double play_time(void);
static int player_control(void) {
    uint64_t statics = rdp(rdp(g_main + OFF_GLOBALS_CLASS) + CLASS_STATICS);
    if (!statics) return 0;
    uint64_t world = rdp(statics + GLOBALS_WORLD);
    return (rd32(statics + GLOBALS_INPUT_ENABLED) & 0xff) && !(world && rd32(world + WORLD_PAUSED));
}
// Reads that cannot fault: a metadata slot holds an encoded token, not a pointer, until IL2CPP initializes it, and
// the objects may not exist yet; 0 for anything unreadable or not a plausible guest pointer.
static int plausible(uint64_t p) { return p >= 0x1000 && p < (1ull << 39) && !(p & 7); }
static uint64_t rd64(uint64_t a) {
    uint64_t v = 0;
    return plausible(a & ~7ull) && plat_mem_read(a, &v, 8) ? v : 0;
}
static uint32_t rd32(uint64_t a) {
    uint32_t v = 0;
    return a >= 0x1000 && a < (1ull << 39) && plat_mem_read(a, &v, 4) ? v : 0;
}
static uint64_t rdp(uint64_t a) {   // a pointer field: 0 unless plausible
    uint64_t v = rd64(a);
    return plausible(v) ? v : 0;
}

// The level table: *n entries, NULL before the game built it.
static uint64_t levels(int *n) {
    uint64_t slot = rd64(g_main + OFF_MAIN_CLASS), cls = plausible(slot) ? slot : 0;
    uint64_t statics = cls && rd32(cls + CLASS_CCTOR_DONE) ? rdp(cls + CLASS_STATICS) : 0;
    uint64_t inst = rdp(statics + MAIN_INSTANCE), common = rdp(inst + MAIN_COMMON_DATA), list = rdp(common + COMMON_LEVELS);
    if (!list) {
        rt_log("warp: no level table yet (Main class slot %#llx, statics %#llx, instance %#llx, common data %#llx)",
               (unsigned long long)slot, (unsigned long long)statics, (unsigned long long)inst, (unsigned long long)common);
        return 0;
    }
    *n = (int)rd32(list + LIST_SIZE);
    return rdp(list + LIST_ITEMS);
}

static uint64_t level_at(uint64_t items, int i) { return rdp(items + ARRAY_DATA + 8 * (uint64_t)i); }

static void level_name(uint64_t lv, char *out, size_t cap) {
    uint64_t s = rdp(lv + LEVEL_NAME);
    size_t n = s ? rd32(s + STRING_LENGTH) : 0, k = 0;
    for (; k < n && k + 1 < cap; k++) {
        uint16_t ch = 0;
        if (!plat_mem_read(s + 0x14 + 2 * k, &ch, 2)) break;
        out[k] = (char)ch;   // the names are ASCII
    }
    out[k] = 0;
}

static uint64_t level_by_hash(uint64_t items, int n, uint32_t hash) {
    for (int i = 0; i < n; i++) {
        uint64_t lv = level_at(items, i);
        if (lv && rd32(lv + LEVEL_NAME_HASH) == hash) return lv;
    }
    return 0;
}

// CB_WARP's level: a WorldID number, else the first level whose name contains the text.
static uint64_t level_wanted(uint64_t items, int n) {
    char *end;
    long id = strtol(g_warp, &end, 10);
    char name[128];
    for (int i = 0; i < n; i++) {
        uint64_t lv = level_at(items, i);
        if (!lv) continue;
        if (!*end && end != g_warp) {
            if ((long)(int32_t)rd32(lv + LEVEL_WORLD_ID) == id) return lv;
        } else {
            level_name(lv, name, sizeof name);
            if (strstr(name, g_warp)) return lv;
        }
    }
    return 0;
}

static int hook_tie_wipe(Cpu *c, void *arg) {
    (void)c, (void)arg;
    int n = 0;
    uint64_t items = levels(&n);
    uint64_t gcls = rdp(g_main + OFF_GLOBALS_CLASS);
    uint64_t state = rdp(rdp(gcls + CLASS_STATICS) + GLOBALS_GAME_STATE);
    if (!items || !state) {
        rt_log("warp: LoadSceneTieWipe without a level table or game state (Globals class %#llx, game state %#llx)",
               (unsigned long long)gcls, (unsigned long long)state);
        return 0;
    }
    uint32_t desired = rd32(state + GAME_STATE_DESIRED);
    uint64_t from = level_by_hash(items, n, desired);
    int32_t from_id = from ? (int32_t)rd32(from + LEVEL_WORLD_ID) : -1;
    if (!from || from_id == WORLD_FRONTEND || from_id == WORLD_ISLANDMAP) return 0;   // not a level launch
    char name[128];
    if (!strcmp(g_warp, "list")) {
        rt_log("warp: the game's level table, %d entries (WorldID, name hash, name):", n);
        for (int i = 0; i < n; i++) {
            uint64_t lv = level_at(items, i);
            if (!lv) continue;
            level_name(lv, name, sizeof name);
            rt_log("warp:   %3d  %08x  %s", (int32_t)rd32(lv + LEVEL_WORLD_ID), rd32(lv + LEVEL_NAME_HASH), name);
        }
    } else {
        uint64_t to = level_wanted(items, n);
        if (!to) rt_fatal("warp: CB_WARP=%s names no level of the game's table (CB_WARP=list lists them)", g_warp);
        uint32_t hash = rd32(to + LEVEL_NAME_HASH);
        ST32(state + GAME_STATE_DESIRED, hash);
        ST32(state + GAME_STATE_CURRENT, hash);
        char was[128];
        level_name(from, was, sizeof was);
        level_name(to, name, sizeof name);
        rt_log("warp: the map launched %s (WorldID %d); loading %s (WorldID %d, hash %08x) instead", was, from_id, name,
               (int32_t)rd32(to + LEVEL_WORLD_ID), hash);
        g_state = state, g_target = hash;
        input_script_end();   // the menus are done: what the script still holds would play in the level
        level_run_warped(name, (int32_t)rd32(to + LEVEL_WORLD_ID), play_time());
    }
    rt_entry_hook_set(g_main + OFF_LOAD_SCENE_TIE_WIPE, NULL, NULL);   // once
    return 0;
}

static int hook_level_summary(Cpu *c, void *arg) {
    (void)c, (void)arg;
    level_run_clear("LevelSummaryUIController.Activate");
    return 0;
}

// get_PlayerCantBeKilled's first call: the flag is set in the CheatsData asset itself, then the getter runs as
// written (and every later call reads the flag without a hook).
static int hook_cant_be_killed(Cpu *c, void *arg) {
    (void)arg;
    if (!c->x[0]) return 0;
    ST8(c->x[0] + CHEATS_CANT_BE_KILLED, 1);
    rt_log("cheats: player cannot be killed (CheatsData %#llx)", (unsigned long long)c->x[0]);
    rt_entry_hook_set(g_main + OFF_CANT_BE_KILLED, NULL, NULL);
    return 0;
}

// CMapManager.LaunchLevel(this, areaNodeId, restartMode): the restart mode the game state takes for the level
// (CGameState.mRestartMode; its level-enter coroutine sets up the Super Guide or Time Attack game mode from it).
static int64_t g_launch_mode = -1;
static int hook_launch_level(Cpu *c, void *arg) {
    (void)arg;
    rt_log("launch: the map launches a level with restart mode %llu; mode %lld instead", (unsigned long long)c->x[2],
           (long long)g_launch_mode);
    c->x[2] = (uint64_t)g_launch_mode;
    rt_entry_hook_set(g_main + OFF_MAP_LAUNCH_LEVEL, NULL, NULL);
    return 0;
}

// The game state's play time (float, seconds) as a double; only its changes matter.
static double play_time(void) {
    uint32_t bits = rd32(g_state + GAME_STATE_PLAY_TIME);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// The end text when the game left the warped level: the level it went to.
static const char *left_for(void) {
    static char where[160];
    snprintf(where, sizeof where, "another world");
    int n = 0;
    uint64_t items = levels(&n), lv = items ? level_by_hash(items, n, rd32(g_state + GAME_STATE_CURRENT)) : 0;
    if (lv) {
        char nm[128];
        level_name(lv, nm, sizeof nm);
        snprintf(where, sizeof where, "left for %s", nm);
    }
    return where;
}

int dkcr_tools_frame(uint64_t frame) {
    if (!g_state || !level_run_active()) return 0;
    return level_run_frame(frame, play_time(), rd32(g_state + GAME_STATE_CURRENT) == g_target, player_control(), left_for);
}

void dkcr_tools_finish(const char *why) { level_run_finish(why); }

void dkcr_tools_init(void) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, "main")) g_main = ldr_module(i)->base;
    if (!g_main) return;
    dkcr_options_init(g_main);   // the PC settings in the game's Options screen (dkcr_options.c)
    // Every hook below is at a Returns HD address: another game (Tropical Freeze shares CB_LAUNCH_MODE) gets none.
    if (rt_program_id() != 0x01009D901BC56000ull) return;
    const char *w = getenv("CB_WARP");
    if (w && *w) {
        g_warp = w;
        rt_entry_hook_set(g_main + OFF_LOAD_SCENE_TIE_WIPE, hook_tie_wipe, NULL);
        rt_entry_hook_set(g_main + OFF_START_LEVEL_SUMMARY, hook_level_summary, NULL);
        rt_log("warp: CB_WARP=%s armed (applies at the first level the island map launches)", w);
    }
    const char *m = getenv("CB_LAUNCH_MODE");
    if (m && *m) {
        g_launch_mode = !strcmp(m, "superguide") ? 5 : !strcmp(m, "timeattack") ? 6 : !strcmp(m, "replay") ? 7 : -1;
        if (g_launch_mode < 0) rt_fatal("CB_LAUNCH_MODE=%s: superguide, timeattack or replay", m);
        rt_entry_hook_set(g_main + OFF_MAP_LAUNCH_LEVEL, hook_launch_level, NULL);
    }
    const char *inv = getenv("CB_INVINCIBLE");
    if (inv && !strcmp(inv, "1")) rt_entry_hook_set(g_main + OFF_CANT_BE_KILLED, hook_cant_be_killed, NULL);
}
