// Country-Box: the PC settings inside Donkey Kong Country: Tropical Freeze's own Options screen (1.0.0). The game's
// planks stay as they are; one more plank, "PC OPTIONS", is a hidden plank of the game's own page made visible, and it
// opens a settings page that is the game's Audio page with our texts. Every row reads and writes the settings model
// (settings.c), the same as the overlay menu and the launcher. Nothing of the game ships with us: the plank, the page,
// the font and the sounds are the player's own, driven at run time through the game's Scaleform API.
//
// How the game builds its Options (measured: JOOptions.swf in romfs UI/UIPak.pak, its AS3 classes read offline, and
// main's code; offsets main+, version 1.0.0). One Scaleform movie, root class shell.Options, serves the pause menu, the
// world map and the title. Its pages are root members: options_main (planks btn0..btn4, Button_square_anim; in a level
// and at the title Audio, Controls and Mode Details are shown and btn3/btn4 are hidden, its private maxSelection is 2),
// options_audio (two rows: txt_fx/toggle_fxVolume and txt_music/toggle_musicVolume, BaseToggle: label above a
// "< value >" plank). Pages change through the menu state in the game's data model: root.setAudio() and root.setMain()
// set it. Input reaches the movie as key events from CScaleformFlashMovieInstance::UserInput (main+0x659e00), which
// asks an IFlashInputProvider per button (vtable slot 0: pressed this frame) through the table at main+0x1588774
// (0 up, 1 down, 2 left, 3 right, 4 A, 6 B).
//
// While our plank is focused or our page is open, UserInput does not run (the movie gets no keys) and the host handles
// the buttons; otherwise the movie runs as always. Our page shows two rows at a time and scrolls through every row of
// this game's settings.
//
//   CB_NATIVE_OPTIONS=1   the plank and the page (off by default)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "settings.h"
#include "ui_text.h"

#define PC_OPTIONS_LABEL "PC OPTIONS"

#define OFF_USER_INPUT 0x659e00ull     // CScaleformFlashMovieInstance::UserInput(IFlashInputProvider const&)
#define OFF_GET_VARIABLE 0x8b6bc8ull   // Scaleform::GFx::Movie::GetVariable(Value*, char const*) const
#define OFF_SET_VARIABLE 0x8b6bd8ull   // Scaleform::GFx::Movie::SetVariable(char const*, Value const&, SetVarType)
#define OFF_INVOKE_FMT 0x8b6c38ull     // Scaleform::GFx::Movie::Invoke(char const*, Value*, char const*, ...)
#define INST_MOVIE(i) LD64(LD64((i) + 8) + 0x18)   // CScaleformFlashMovieInstance -> its GFx::Movie (GetMovie)

// Scaleform::GFx::Value as this build lays it out (CScaleformFlashMovieInstance::RegisterDirectAccessCallback builds
// them on its stack): a list node (two words, -1 when unlinked), the object interface (managed values), the type, the
// payload. Type & 0x40: managed (released through the interface); & 0x8f: the kind.
typedef struct {
    uint64_t prev, next, iface;
    uint32_t type, pad;
    uint64_t v;
} GfxValue;
enum { VT_BOOL = 2, VT_INT = 3, VT_STRING = 6, VT_MANAGED = 0x40 };
enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B = 6 };

static uint64_t g_main;
static int g_on, g_trace;   // CB_NATIVE_OPTIONS_TRACE=1: each UserInput call of the Options movie logged

static uint64_t g_movie;   // the Options movie last seen (its GFx::Movie)
static enum { S_NONE, S_PLANK, S_OPENING, S_PAGE, S_CLOSING } g_state;
static int g_slot = -1, g_last = -1;   // our plank btn<g_slot>; the game's last shown plank
static int g_cursor, g_top, g_nrows;
static const SettingRow *g_rows[64];

static void value_release(Cpu *c, GfxValue *v) {
    if ((v->type & VT_MANAGED) && v->iface) rt_call(c, LD64(LD64(v->iface) + 0x18), 3, v->iface, (uint64_t)(uintptr_t)v, v->v);
    v->iface = 0, v->type = 0;
}

// Where the Options root (shell.Options) sits in the movie: the pause menu loads JOOptions.swf into
// root.levelMenu.Hscroll.container_options (measured: its child instance1), the map into its own container. Found by
// search (find_base) and kept while it answers. Paths not starting with "root" are relative to it.
static char g_base[256];
static const char *full(const char *path) {
    static char buf[2][512];
    static int k;
    if (!strncmp(path, "root", 4)) return path;
    k ^= 1;
    snprintf(buf[k], sizeof buf[k], "%s.%s", g_base, path);
    return buf[k];
}

// The value at path ("a.b.c"), to be released (value_release); 0 when there is none.
static int get_var(Cpu *c, uint64_t movie, const char *path, GfxValue *out) {
    *out = (GfxValue){.prev = ~0ull, .next = ~0ull};
    path = full(path);
    return (int)(rt_call(c, g_main + OFF_GET_VARIABLE, 3, movie, (uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)path) & 1);
}

static int get_bool(Cpu *c, uint64_t movie, const char *path, int *b) {
    GfxValue v;
    int ok = get_var(c, movie, path, &v) && (v.type & 0x8f) == VT_BOOL;
    if (ok) *b = (int)(v.v & 0xff);
    value_release(c, &v);
    return ok;
}

static int get_str(Cpu *c, uint64_t movie, const char *path, char *out, size_t cap) {
    GfxValue v;
    int ok = get_var(c, movie, path, &v) && (v.type & 0x8f) == VT_STRING;
    out[0] = 0;
    if (ok) {
        const char *s = (const char *)(uintptr_t)((v.type & VT_MANAGED) ? LD64(v.v) : v.v);
        snprintf(out, cap, "%s", s ? s : "");
    }
    value_release(c, &v);
    return ok;
}

static void set_bool(Cpu *c, uint64_t movie, const char *path, int b) {
    GfxValue v = {.prev = ~0ull, .next = ~0ull, .type = VT_BOOL, .v = (uint64_t)!!b};
    path = full(path);
    rt_call(c, g_main + OFF_SET_VARIABLE, 4, movie, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&v, 0ull);
}

static void set_text(Cpu *c, uint64_t movie, const char *path, const char *text) {
    GfxValue v = {.prev = ~0ull, .next = ~0ull, .type = VT_STRING, .v = (uint64_t)(uintptr_t)text};
    path = full(path);
    rt_call(c, g_main + OFF_SET_VARIABLE, 4, movie, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&v, 0ull);
}

// path(fmt args...): an AS3 method of the movie; fmt "" (none), "%s" or "%d".
static void invoke0(Cpu *c, uint64_t movie, const char *path) {
    path = full(path);
    rt_call(c, g_main + OFF_INVOKE_FMT, 4, movie, (uint64_t)(uintptr_t)path, 0ull, (uint64_t)(uintptr_t)"");
}
static void invoke_s(Cpu *c, uint64_t movie, const char *path, const char *s) {
    path = full(path);
    rt_call(c, g_main + OFF_INVOKE_FMT, 5, movie, (uint64_t)(uintptr_t)path, 0ull, (uint64_t)(uintptr_t)"%s",
            (uint64_t)(uintptr_t)s);
}
static void invoke_d(Cpu *c, uint64_t movie, const char *path, int d) {
    path = full(path);
    rt_call(c, g_main + OFF_INVOKE_FMT, 5, movie, (uint64_t)(uintptr_t)path, 0ull, (uint64_t)(uintptr_t)"%d", (uint64_t)d);
}

static void plank_state(Cpu *c, uint64_t movie, int i, const char *state) {
    char p[64];
    snprintf(p, sizeof p, "options_main.btn%d.setState", i);
    invoke_s(c, movie, p, state);
}

static int plank_visible(Cpu *c, uint64_t movie, int i) {
    char p[64];
    int b = 0;
    snprintf(p, sizeof p, "options_main.btn%d.visible", i);
    return get_bool(c, movie, p, &b) && b;
}

static int plank_highlighted(Cpu *c, uint64_t movie, int i) {
    char p[64], l[64];
    snprintf(p, sizeof p, "options_main.btn%d.currentLabel", i);
    if (!get_str(c, movie, p, l, sizeof l)) return 0;
    return !strcmp(l, "highlighted") || !strcmp(l, "startHighlighted");
}

// CB_NATIVE_OPTIONS_TRACE: the display list under path, names and child counts, depth levels deep.
static int get_num(Cpu *c, uint64_t movie, const char *path, double *d) {
    GfxValue v;
    int ok = get_var(c, movie, path, &v), t = v.type & 0x8f;
    if (ok && t == VT_INT) *d = (double)(int32_t)v.v;
    else if (ok && t == 4) *d = (double)(uint32_t)v.v;
    else if (ok && t == 5) memcpy(d, &v.v, 8);
    else ok = 0;
    value_release(c, &v);
    return ok;
}
static void dump_tree(Cpu *c, uint64_t movie, const char *path, int depth) {
    double n = 0;
    char p[256];
    snprintf(p, sizeof p, "%s.numChildren", path);
    if (!get_num(c, movie, p, &n)) { rt_log("options: tree %s (no numChildren)", path); return; }
    rt_log("options: tree %s: %d children", path, (int)n);
    for (int i = 0; i < (int)n && i < 24; i++) {
        GfxValue res = {.prev = ~0ull, .next = ~0ull};
        snprintf(p, sizeof p, "%s.getChildAt", path);
        rt_call(c, g_main + OFF_INVOKE_FMT, 5, movie, (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)&res,
                (uint64_t)(uintptr_t)"%d", (uint64_t)i);
        char name[96] = "?";
        if ((res.type & 0x8f) >= 8 && res.iface) {
            GfxValue nm = {.prev = ~0ull, .next = ~0ull};
            int ok = (int)(rt_call(c, LD64(LD64(res.iface) + 0x28), 5, res.iface, res.v, (uint64_t)(uintptr_t)"name",
                                   (uint64_t)(uintptr_t)&nm, (uint64_t)((res.type & 0x8f) == 0xa)) & 1);
            if (ok && (nm.type & 0x8f) == VT_STRING)
                snprintf(name, sizeof name, "%s", (const char *)(uintptr_t)((nm.type & VT_MANAGED) ? LD64(nm.v) : nm.v));
            value_release(c, &nm);
        }
        rt_log("options: tree %s[%d] = %s (type %#x)", path, i, name, res.type);
        value_release(c, &res);
        if (depth > 1 && strcmp(name, "?")) {
            char q[256];
            snprintf(q, sizeof q, "%s.%s", path, name);
            dump_tree(c, movie, q, depth - 1);
        }
    }
}

// The name of child i of the display object at path ("" when none).
static void child_name(Cpu *c, uint64_t movie, const char *path, int i, char *name, size_t cap) {
    GfxValue res = {.prev = ~0ull, .next = ~0ull};
    char p[512];
    snprintf(p, sizeof p, "%s.getChildAt", path);
    rt_call(c, g_main + OFF_INVOKE_FMT, 5, movie, (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)&res, (uint64_t)(uintptr_t)"%d",
            (uint64_t)i);
    name[0] = 0;
    if ((res.type & 0x8f) >= 8 && res.iface) {
        GfxValue nm = {.prev = ~0ull, .next = ~0ull};
        int ok = (int)(rt_call(c, LD64(LD64(res.iface) + 0x28), 5, res.iface, res.v, (uint64_t)(uintptr_t)"name",
                               (uint64_t)(uintptr_t)&nm, (uint64_t)((res.type & 0x8f) == 0xa)) & 1);
        if (ok && (nm.type & 0x8f) == VT_STRING)
            snprintf(name, cap, "%s", (const char *)(uintptr_t)((nm.type & VT_MANAGED) ? LD64(nm.v) : nm.v));
        value_release(c, &nm);
    }
    value_release(c, &res);
}

// Depth-first search under path for the clip holding options_main (at most depth levels); 1 and g_base set when found.
static int find_base(Cpu *c, uint64_t movie, const char *path, int depth) {
    char p[512];
    GfxValue v;
    snprintf(p, sizeof p, "%s.options_main", path);
    int ok = get_var(c, movie, p, &v) && (v.type & 0x8f) == 0xa;   // a display object (a TextField answered too)
    value_release(c, &v);
    if (ok) {
        snprintf(g_base, sizeof g_base, "%s", path);
        return 1;
    }
    double n = 0;
    snprintf(p, sizeof p, "%s.numChildren", path);
    if (depth <= 0 || !get_num(c, movie, p, &n)) return 0;
    for (int i = 0; i < (int)n && i < 32; i++) {
        char name[96];
        child_name(c, movie, path, i, name, sizeof name);
        if (!name[0]) continue;
        snprintf(p, sizeof p, "%s.%s", path, name);
        if (find_base(c, movie, p, depth - 1)) return 1;
    }
    return 0;
}

// ---- the rows
static const char *en(UiTextId id) { return ui_text_in("en", id); }

static void row_value(const SettingRow *r, char *out, size_t cap) {
    if (r->unavailable != SETTING_LITERAL) {
        snprintf(out, cap, "N/A");
        return;
    }
    if (r->kind == SETTING_RANGE) {
        snprintf(out, cap, "%g%s", cfg_double(r->cfg), r->unit);
        return;
    }
    const SettingOption *o = settings_current(r);
    if (!o) snprintf(out, cap, "%s", cfg_value(r->cfg));
    else if (o->text == SETTING_LITERAL) snprintf(out, cap, "%s", o->literal ? o->literal : o->value);
    else snprintf(out, cap, "%s", en(o->text));
}

static void row_label(const SettingRow *r, char *out, size_t cap) {
    const CfgMeta *m = cfg_meta(r->cfg);
    snprintf(out, cap, "%s%s", en(r->label), m && !m->live ? " *" : "");   // *: applies at the next start
}

static void step_row(const SettingRow *r, int dir) {
    if (r->unavailable != SETTING_LITERAL) return;
    char v[64];
    if (r->kind == SETTING_RANGE) {
        double d = cfg_double(r->cfg) + dir * r->step;
        d = d < r->lo ? r->lo : d > r->hi ? r->hi : d;
        snprintf(v, sizeof v, "%g", d);
        settings_set(r, v);
    } else {
        const SettingOption *cur = settings_current(r);
        int k = cur ? (int)(cur - r->opts) : 0;
        for (int j = k + dir; j >= 0 && j < r->n_opts; j += dir)
            if (r->opts[j].unavailable == SETTING_LITERAL) {
                settings_set(r, r->opts[j].value);
                break;
            }
    }
    cfg_save();
    row_value(r, v, sizeof v);
    rt_log("options: %s = %s", r->key, v);
}

static void fill_page(Cpu *c, uint64_t movie) {
    static char title[64], lab[2][96], val[2][64];   // the movie copies them; static so they outlive the calls
    snprintf(title, sizeof title, PC_OPTIONS_LABEL " %d/%d", g_cursor + 1, g_nrows);
    set_text(c, movie, "options_audio.txt_title.text", title);
    static const char *const lab_path[2] = {"options_audio.txt_fx.text", "options_audio.txt_music.text"};
    static const char *const tog[2] = {"options_audio.toggle_fxVolume", "options_audio.toggle_musicVolume"};
    for (int i = 0; i < 2; i++) {
        char p[96];
        int k = g_top + i;
        const SettingRow *r = k < g_nrows ? g_rows[k] : NULL;
        if (r) row_label(r, lab[i], sizeof lab[i]), row_value(r, val[i], sizeof val[i]);
        else lab[i][0] = 0, val[i][0] = 0;
        set_text(c, movie, lab_path[i], lab[i]);
        snprintf(p, sizeof p, "%s.setToggleText", tog[i]);
        invoke_s(c, movie, p, val[i]);
        snprintf(p, sizeof p, "%s.setHighlight", tog[i]);
        invoke_d(c, movie, p, k == g_cursor);
        int live = r && r->unavailable == SETTING_LITERAL;
        snprintf(p, sizeof p, "%s.setArrowLeftEnabled", tog[i]);
        invoke_d(c, movie, p, live);
        snprintf(p, sizeof p, "%s.setArrowRightEnabled", tog[i]);
        invoke_d(c, movie, p, live);
    }
}

// ---- the frame: UserInput of a Scaleform movie (this: CScaleformFlashMovieInstance, x1: IFlashInputProvider)
static int pressed(Cpu *c, uint64_t provider, int key) {
    return (int)(rt_call(c, LD64(LD64(provider)), 2, provider, (uint64_t)key) & 1);
}

static int user_input(Cpu *c) {
    uint64_t inst = arg_x(c, 0), provider = arg_x(c, 1), movie = INST_MOVIE(inst);
    if (!movie) return 0;
    int main_vis = 0, audio_vis = 0;
    if (movie != g_movie) g_movie = movie, g_state = S_NONE, g_slot = -1, g_base[0] = 0;
    if (!g_base[0] || !get_bool(c, movie, "options_main.visible", &main_vis)) {   // the Options clip moved or went
        static int tries;
        g_base[0] = 0;
        if (g_state != S_NONE || ++tries % 4 == 0) {
            // the pause menu's container (measured), the map's and the title's by the same name, then the root itself
            static const char *const where[] = {"root.levelMenu.Hscroll.container_options", "root.mapMenu.Vscroll.container_options",
                                                "root.container_options", "root"};
            int found = 0;
            for (size_t i = 0; i < sizeof where / sizeof *where && !found; i++) found = find_base(c, movie, where[i], 2);
            if (!found && g_trace && tries % 64 == 0) found = find_base(c, movie, "root", 5);
            if (!found) { g_state = S_NONE; return 0; }
            if (g_trace) rt_log("options: Options root at %s", g_base);
        } else
            return 0;
    }
    if (g_trace && LD32(provider + 8) == K_A && LD32(provider + 12) == 0) {
        static int dumps;
        if (dumps++ < 3) {
            dump_tree(c, movie, "root.levelMenu.Hscroll.container_options", 4);
            static const char *const probe[] = {"root.levelMenu.Hscroll.container_options.content",
                                                "root.levelMenu.Hscroll.container_options.content.options_main",
                                                "root.levelMenu.Hscroll.container_options.content.options_main.visible"};
            for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) {
                GfxValue v;
                int ok = get_var(c, movie, probe[i], &v);
                rt_log("options: GetVariable(%s) = %d type %#x", probe[i], ok, v.type);
                value_release(c, &v);
            }
        }
    }
    if (g_trace) {
        static uint64_t seen[16];
        static int nseen;
        int k = 0;
        while (k < nseen && seen[k] != movie) k++;
        if (k == nseen && nseen < 16) {
            seen[nseen++] = movie;
            static const char *const probe[] = {"options_main", "root.options_main", "_root.options_main", "root", "stage",
                                                "options_main.visible"};
            for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) {
                GfxValue v;
                int ok = get_var(c, movie, probe[i], &v);
                rt_log("options: movie %#llx GetVariable(%s) = %d type %#x", (unsigned long long)movie, probe[i], ok, v.type);
                value_release(c, &v);
            }
            dump_tree(c, movie, "root", 1);
        }
    }
    if (!get_bool(c, movie, "options_main.visible", &main_vis)) { g_state = S_NONE; return 0; }
    get_bool(c, movie, "options_audio.visible", &audio_vis);
    if (g_trace) {
        char l[3][32];
        for (int i = 0; i < 3; i++) {
            char p[64];
            snprintf(p, sizeof p, "options_main.btn%d.currentLabel", i);
            if (!get_str(c, movie, p, l[i], sizeof l[i])) snprintf(l[i], sizeof l[i], "?");
        }
        rt_log("options: input cmd %u/%u state %d main %d audio %d planks %d%d%d%d%d labels %s %s %s", LD32(provider + 8),
               LD32(provider + 12), g_state, main_vis, audio_vis, plank_visible(c, movie, 0), plank_visible(c, movie, 1),
               plank_visible(c, movie, 2), plank_visible(c, movie, 3), plank_visible(c, movie, 4), l[0], l[1], l[2]);
    }
    switch (g_state) {
    case S_NONE: {
        if (!main_vis) return 0;
        int last = -1;
        for (int i = 0; i < 5; i++)
            if (i != g_slot && plank_visible(c, movie, i)) last = i;
        if (last < 0 || last >= 4) return 0;   // no hidden plank below the game's last one
        int slot = last + 1;
        if (slot != g_slot || !plank_visible(c, movie, slot)) {   // (again) after the page's initMenu hid it
            char p[64];
            snprintf(p, sizeof p, "options_main.btn%d.visible", slot);
            set_bool(c, movie, p, 1);
            snprintf(p, sizeof p, "options_main.btn%d.text", slot);
            set_text(c, movie, p, PC_OPTIONS_LABEL);
            plank_state(c, movie, slot, "Unpressed");
            g_slot = slot;
        }
        g_last = last;
        if (pressed(c, provider, K_DOWN) && plank_highlighted(c, movie, last)) {   // down from the last plank: ours
            plank_state(c, movie, last, "Unpressed");
            plank_state(c, movie, g_slot, "Highlighted");
            g_state = S_PLANK;
            return 1;
        }
        return 0;
    }
    case S_PLANK:
        if (!main_vis) { g_state = S_NONE; return 0; }
        if (pressed(c, provider, K_UP)) {
            plank_state(c, movie, g_slot, "Unpressed");
            plank_state(c, movie, g_last, "Highlighted");
            g_state = S_NONE;
        } else if (pressed(c, provider, K_DOWN)) {   // the game's own step from its last plank wraps to the first
            plank_state(c, movie, g_slot, "Unpressed");
            invoke_d(c, movie, "options_main.nextItem", 0);
            g_state = S_NONE;
        } else if (pressed(c, provider, K_A)) {
            plank_state(c, movie, g_slot, "Pressed");
            g_nrows = 0;
            for (int i = 0; settings_row(i) && g_nrows < (int)(sizeof g_rows / sizeof *g_rows); i++)
                g_rows[g_nrows++] = settings_row(i);
            g_cursor = g_top = 0;
            invoke0(c, movie, "setAudio");
            g_state = S_OPENING;
        } else if (pressed(c, provider, K_B)) {   // the game's back, from its own last plank
            plank_state(c, movie, g_slot, "Unpressed");
            plank_state(c, movie, g_last, "Highlighted");
            g_state = S_NONE;
            return 0;
        }
        return 1;
    case S_OPENING:
        if (!audio_vis) return 1;
        g_state = S_PAGE;
        fill_page(c, movie);
        return 1;
    case S_PAGE: {
        if (!audio_vis) { g_state = S_NONE; return 0; }
        int dirty = 0;
        if (pressed(c, provider, K_UP)) g_cursor = (g_cursor + g_nrows - 1) % g_nrows, dirty = 1;
        else if (pressed(c, provider, K_DOWN)) g_cursor = (g_cursor + 1) % g_nrows, dirty = 1;
        else if (pressed(c, provider, K_LEFT)) step_row(g_rows[g_cursor], -1), dirty = 1;
        else if (pressed(c, provider, K_RIGHT)) step_row(g_rows[g_cursor], +1), dirty = 1;
        else if (pressed(c, provider, K_B)) {
            invoke0(c, movie, "setMain");
            g_state = S_CLOSING;
            return 1;
        }
        if (g_cursor < g_top) g_top = g_cursor;
        if (g_cursor > g_top + 1) g_top = g_cursor - 1;
        static int every;
        if (dirty || ++every % 15 == 0) fill_page(c, movie);   // the page's own frame handler may redraw its rows
        return 1;
    }
    case S_CLOSING:
        if (!main_vis) return 1;
        // back on the planks: ours keeps the focus, as the game's own pages give it back to theirs
        if (g_slot >= 0) {
            char p[64];
            snprintf(p, sizeof p, "options_main.btn%d.visible", g_slot);
            set_bool(c, movie, p, 1);
            snprintf(p, sizeof p, "options_main.btn%d.text", g_slot);
            set_text(c, movie, p, PC_OPTIONS_LABEL);
            plank_state(c, movie, g_last, "Unpressed");
            plank_state(c, movie, g_slot, "Highlighted");
            g_state = S_PLANK;
        } else
            g_state = S_NONE;
        return 1;
    }
    return 0;
}

// The calls into the movie above (rt_call) leave the argument and scratch registers changed: when the game's UserInput
// runs after them it must find its own (this, provider) and the rest as they were.
static int hook_user_input(Cpu *c, void *arg) {
    (void)arg;
    uint64_t x[31], sp = c->sp;
    __uint128_t v[32];
    memcpy(x, c->x, sizeof x), memcpy(v, c->v, sizeof v);
    int skip = user_input(c);
    memcpy(c->x, x, sizeof x), memcpy(c->v, v, sizeof v);
    c->sp = sp;
    return skip;   // skipped: UserInput returns nothing
}

void dktf_options_init(uint64_t main_base) {
    const char *e = getenv("CB_NATIVE_OPTIONS");
    g_on = e && !strcmp(e, "1");
    g_trace = getenv("CB_NATIVE_OPTIONS_TRACE") != NULL;
    if (!g_on || settings_game() != SETTINGS_GAME_DKCTF) return;
    g_main = main_base;
    rt_entry_hook_set(g_main + OFF_USER_INPUT, hook_user_input, NULL);
    rt_log("options: PC OPTIONS in the game's Options screen (CB_NATIVE_OPTIONS=1)");
}
