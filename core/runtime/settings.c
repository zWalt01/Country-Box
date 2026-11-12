// Country-Box: the player-facing settings model (settings.h): the table of rows, this machine's defaults, and the
// command line the launcher uses (--settings-dump, --settings-set).
//
// What each row does in each game was checked on Returns HD 1.0.0 (docs/LAUNCHER.md, "Modelo de ajustes"). Rows of
// the inherited Metroid Prime 4 menu that do nothing in these games (its frame-rate patches, its 4K patch, the game
// language: Returns HD has its own in its Options screen, mouse camera, gyro aiming) are not in the table; their cfg
// keys stay for the Advanced tab and the environment.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cfg.h"
#include "platform/platform.h"
#include "settings.h"

#define NA SETTING_LITERAL   // available
#define OPT(v, s, t) {v, s, t, NULL, NA}
#define OPT_NA(v, s, t, why) {v, s, t, NULL, why}
#define LIT(v, s, label) {v, s, SETTING_LITERAL, label, NA}
#define N(a) (int)(sizeof a / sizeof *a)

static const SettingOption o_display_mode[] = {
    OPT("windowed", "0", UI_SET_MODE_WINDOWED), OPT("borderless", "1", UI_SET_MODE_BORDERLESS),
    OPT_NA("exclusive", NULL, UI_SET_MODE_EXCLUSIVE, UI_SET_NA_EXCLUSIVE),
};
// 16:9 sizes and the Steam Deck's 16:10 panel; the window row's list (ui/menu.cpp) adds the current size when another
static const SettingOption o_window_size[] = {
    LIT("1280x720", "1280x720", "1280 × 720"), LIT("1280x800", "1280x800", "1280 × 800"), LIT("1366x768", "1366x768", "1366 × 768"),
    LIT("1600x900", "1600x900", "1600 × 900"), LIT("1920x1080", "1920x1080", "1920 × 1080"), LIT("2560x1440", "2560x1440", "2560 × 1440"),
    LIT("3200x1800", "3200x1800", "3200 × 1800"), LIT("3840x2160", "3840x2160", "3840 × 2160"),
};
// The console's own two resolutions: the game draws 1920x1080 docked and 1280x720 handheld (ov_system.c reports
// the operation mode; Unity sizes its framebuffer from it). Measured on level 13 (2026-11-11): handheld gives a 1280x720
// picture with the same framing, HUD layout and detail, FSR 1 upscales it. The Steam Deck takes 720p: its 1280x800
// panel shows it 1:1 (docked would be scaled down).
static const SettingOption o_render_res[] = {
    OPT("1920x1080", "docked", UI_SET_RES_1080), OPT("1280x720", "handheld", UI_SET_RES_720),
};
static const SettingOption o_render_scale[] = {
    LIT("100", "game", "100 %"),
};
static const SettingOption o_upscaler[] = {
    OPT("fsr", "fsr", UI_MENU_UPSCALER_FSR), OPT("bilinear", "bilinear", UI_MENU_UPSCALER_BILINEAR),
};
static const SettingOption o_aspect[] = {   // Tropical Freeze draws 16:10 itself (dktf_aspect.c, read at start)
    OPT("fit", "fit", UI_MENU_ASPECT_FIT), OPT("stretch", "stretch", UI_MENU_ASPECT_STRETCH),
    OPT("native", "native", UI_MENU_ASPECT_NATIVE),
};
static const SettingOption o_aspect_r[] = {
    OPT("fit", "fit", UI_MENU_ASPECT_FIT), OPT("stretch", "stretch", UI_MENU_ASPECT_STRETCH),
    OPT_NA("native", "native", UI_MENU_ASPECT_NATIVE, UI_SET_NA_NATIVE_ASPECT),
};
static const SettingOption o_anisotropy[] = {
    OPT("game", "game", UI_MENU_ANISOTROPY_GAME), LIT("2", "2", "2×"), LIT("4", "4", "4×"), LIT("8", "8", "8×"), LIT("16", "16", "16×"),
};
static const SettingOption o_vsync[] = {
    OPT("mailbox", "mailbox", UI_SET_VSYNC_MAILBOX), OPT("fifo", "fifo", UI_SET_VSYNC_FIFO),
    OPT("immediate", "immediate", UI_SET_VSYNC_OFF),
};
// VARIABLE / 60 / 90: 90 is the Steam Deck OLED panel's rate; both wait for the real-time step
static const SettingOption o_refresh[] = {
    OPT("60", "60", UI_SET_REFRESH_60), OPT_NA("90", "90", UI_SET_REFRESH_90, UI_SET_NA_FIXED_STEP),
    OPT_NA("variable", "variable", UI_SET_REFRESH_VARIABLE, UI_SET_NA_FIXED_STEP),
};
static const SettingOption o_perf[] = {
    OPT("off", "off", UI_MENU_PERF_OFF), OPT("fps", "fps", UI_MENU_PERF_FPS), OPT("full", "full", UI_MENU_PERF_FULL),
};
static const SettingOption o_pad_layout[] = {
    OPT("auto", "auto", UI_MENU_PAD_LAYOUT_AUTO), OPT("nintendo", "nintendo", UI_MENU_PAD_LAYOUT_NINTENDO),
    OPT("xbox", "xbox", UI_MENU_PAD_LAYOUT_XBOX), OPT("xbox_prompts", "xbox_prompts", UI_MENU_PAD_LAYOUT_XBOX_PROMPTS),
};
static const SettingOption o_on_off[] = {OPT("off", "0", UI_MENU_OFF), OPT("on", "1", UI_MENU_ON)};
static const SettingOption o_menu_lang[] = {
    OPT("auto", "", UI_MENU_MENU_LANG_AUTO), OPT("en", "en", UI_MENU_LANGNAME_EN), OPT("ru", "ru", UI_MENU_LANGNAME_RU),
};

#define CHOICE(o) SETTING_CHOICE, 0, 0, o, N(o), 0, 0, 0, NULL
#define RANGE(lo, hi, step, unit) SETTING_RANGE, 0, 0, NULL, 0, lo, hi, step, unit
enum { R = SETTINGS_GAME_DKCR, TF = SETTINGS_GAME_DKCTF, ALL = SETTINGS_GAME_ALL };

// Table order is page order. The kind macros leave label and desc to the two fields after them.
static SettingRow rows[] = {
    {"video.display_mode", "MP4_FULLSCREEN", "video", ALL, CHOICE(o_display_mode), "windowed", "borderless", "borderless", NA},
    {"video.window_size", "MP4_WINDOW_SIZE", "video", ALL, CHOICE(o_window_size), "1920x1080", "1280x800", "1280x800", NA},
    {"video.render_resolution", "MP4_OPERATION_MODE", "video", ALL, CHOICE(o_render_res), "1920x1080", "1280x720", "1280x720", NA},
    {"video.render_scale", "MP4_RENDER_SCALE", "video", R, CHOICE(o_render_scale), "100", NULL, NULL, UI_SET_NA_RENDER_SCALE},
    {"video.upscaler", "MP4_UPSCALER", "video", ALL, CHOICE(o_upscaler), "fsr", NULL, NULL, NA},
    {"video.sharpness", "MP4_SHARPNESS", "video", ALL, RANGE(0, 100, 5, "%"), "80", NULL, NULL, NA},
    {"video.aspect", "MP4_ASPECT", "video", R, CHOICE(o_aspect_r), "fit", NULL, NULL, NA},
    {"video.aspect", "MP4_ASPECT", "video", TF, CHOICE(o_aspect), "fit", NULL, NULL, NA},
    {"video.anisotropy", "MP4_ANISOTROPY", "video", ALL, CHOICE(o_anisotropy), "game", NULL, NULL, NA},
    {"video.vsync", "MP4_VK_PRESENT", "video", ALL, CHOICE(o_vsync), "mailbox", NULL, NULL, NA},
    {"video.refresh", "MP4_REFRESH", "video", ALL, CHOICE(o_refresh), "60", NULL, NULL, NA},
    {"video.perf_overlay", "MP4_PERF_OVERLAY", "video", ALL, CHOICE(o_perf), "off", NULL, NULL, NA},
    {"sound.volume", "MP4_VOLUME", "sound", ALL, RANGE(0, 100, 5, "%"), "100", NULL, NULL, NA},
    {"controls.pad_layout", "MP4_PAD_LAYOUT", "controls", ALL, CHOICE(o_pad_layout), "auto", NULL, NULL, NA},
    {"controls.rumble_strength", "MP4_RUMBLE_STRENGTH", "controls", ALL, RANGE(0, 300, 10, "%"), "130", NULL, NULL, NA},
    {"controls.rumble_curve", "MP4_RUMBLE_CURVE", "controls", ALL, RANGE(0.2f, 1.5f, 0.05f, ""), "0.6", NULL, NULL, NA},
    {"controls.rumble_triggers", "MP4_RUMBLE_TRIGGERS", "controls", ALL, CHOICE(o_on_off), "off", NULL, NULL, NA},
    {"controls.stick_deadzone", "MP4_STICK_DEADZONE", "controls", ALL, RANGE(0, 50, 1, "%"), "8", NULL, NULL, NA},
    {"interface.menu_language", "MP4_MENU_LANG", "interface", ALL, CHOICE(o_menu_lang), "auto", NULL, NULL, NA},
};

// label and desc per row, in table order (kept apart so the table above stays one line per row)
static const UiTextId row_text[][2] = {
    {UI_SET_DISPLAY_MODE, UI_SET_DISPLAY_MODE_DESC}, {UI_MENU_WINDOW_SIZE, UI_SET_WINDOW_SIZE_DESC},
    {UI_SET_RENDER_RES, UI_SET_RENDER_RES_DESC}, {UI_MENU_RENDER_SCALE, UI_SET_RENDER_SCALE_DESC},
    {UI_MENU_UPSCALER, UI_SET_UPSCALER_DESC}, {UI_MENU_SHARPNESS, UI_SET_SHARPNESS_DESC},
    {UI_MENU_ASPECT, UI_MENU_ASPECT_DESC}, {UI_MENU_ASPECT, UI_MENU_ASPECT_DESC},
    {UI_MENU_ANISOTROPY, UI_MENU_ANISOTROPY_DESC},
    {UI_SET_VSYNC, UI_SET_VSYNC_DESC}, {UI_SET_REFRESH, UI_SET_REFRESH_DESC},
    {UI_MENU_PERF_OVERLAY, UI_MENU_PERF_OVERLAY_DESC},
    {UI_MENU_VOLUME, UI_MENU_VOLUME_DESC},
    {UI_MENU_PAD_LAYOUT, UI_MENU_PAD_LAYOUT_DESC}, {UI_MENU_RUMBLE_STRENGTH, UI_MENU_RUMBLE_STRENGTH_DESC},
    {UI_MENU_RUMBLE_CURVE, UI_MENU_RUMBLE_CURVE_DESC}, {UI_MENU_RUMBLE_TRIGGERS, UI_MENU_RUMBLE_TRIGGERS_DESC},
    {UI_MENU_STICK_DEADZONE, UI_MENU_STICK_DEADZONE_DESC},
    {UI_MENU_MENU_LANG, UI_MENU_MENU_LANG_DESC},
};
_Static_assert(sizeof row_text / sizeof *row_text == sizeof rows / sizeof *rows, "a row without its texts");

// Short names, for places with little room (the rows in the game's own Options, dkcr_options.c: a label fits about 8
// letters, a value about 5; the launcher may use them too): a row's label, and "key=value" for an option. English;
// the game's font draws capitals.
static const struct { const char *key, *text; } shorts[] = {
    {"video.display_mode", "MODE"}, {"video.display_mode=windowed", "WIN"}, {"video.display_mode=borderless", "FULL"},
    {"video.window_size", "WINDOW"}, {"video.render_resolution", "RENDER"}, {"video.render_resolution=1920x1080", "1080P"},
    {"video.render_resolution=1280x720", "720P"}, {"video.render_scale", "3D SCALE"}, {"video.upscaler", "SCALING"},
    {"video.upscaler=fsr", "FSR 1"}, {"video.upscaler=bilinear", "BASIC"}, {"video.sharpness", "SHARPEN"},
    {"video.aspect", "SHAPE"}, {"video.aspect=fit", "BARS"}, {"video.aspect=stretch", "FILL"}, {"video.aspect=native", "16:10"},
    {"video.anisotropy", "TEXTURES"}, {"video.anisotropy=game", "GAME"}, {"video.vsync", "VSYNC"},
    {"video.vsync=mailbox", "FAST"}, {"video.vsync=fifo", "ON"}, {"video.vsync=immediate", "OFF"},
    {"video.refresh", "REFRESH"}, {"video.refresh=60", "60 HZ"}, {"video.refresh=90", "90 HZ"},
    {"video.refresh=variable", "VRR"}, {"video.perf_overlay", "OVERLAY"}, {"video.perf_overlay=off", "OFF"},
    {"video.perf_overlay=fps", "FPS"}, {"video.perf_overlay=full", "FULL"}, {"sound.volume", "VOLUME"},
    {"controls.pad_layout", "BUTTONS"}, {"controls.pad_layout=auto", "AUTO"}, {"controls.pad_layout=nintendo", "SWITCH"},
    {"controls.pad_layout=xbox", "XBOX"}, {"controls.pad_layout=xbox_prompts", "XBOX+"},
    {"controls.rumble_strength", "RUMBLE"}, {"controls.rumble_curve", "CURVE"},
    {"controls.rumble_triggers", "TRIGGERS"}, {"controls.rumble_triggers=off", "OFF"}, {"controls.rumble_triggers=on", "ON"},
    {"controls.stick_deadzone", "DEADZONE"}, {"interface.menu_language", "MENU"},
    {"interface.menu_language=auto", "AUTO"}, {"interface.menu_language=en", "EN"}, {"interface.menu_language=ru", "RU"},
};

static const char *short_of(const char *k) {
    for (size_t i = 0; i < sizeof shorts / sizeof *shorts; i++)
        if (!strcmp(shorts[i].key, k)) return shorts[i].text;
    return NULL;
}

const char *settings_short_label(const SettingRow *r) {
    const char *t = short_of(r->key);
    return t ? t : ui_text_in("en", r->label);
}

const char *settings_short_value(const SettingRow *r, const SettingOption *o) {
    char k[160];
    snprintf(k, sizeof k, "%s=%s", r->key, o->value);
    return short_of(k);
}

static const struct { const char *id; UiTextId label; } groups[] = {
    {"video", UI_SET_GROUP_VIDEO}, {"sound", UI_SET_GROUP_SOUND}, {"controls", UI_SET_GROUP_CONTROLS},
    {"interface", UI_SET_GROUP_INTERFACE},
};

// ---- the game and the machine, once
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unsigned g_game = SETTINGS_GAME_DKCR;
static int g_machine = SETTINGS_PC;
// program ids (main.npdm ACI0 +0x10), measured on the retail dumps
#define PID_DKCR 0x01009D901BC56000ull
#define PID_DKCTF 0x0100C1F0051B6000ull

extern const char rt_default_exefs[];
extern const char rt_port_version[];   // Country-Box's VERSION (build_game.py port_version)

static int read_pid(const char *dir, uint64_t *pid) {
    char path[1200];
    snprintf(path, sizeof path, "%s/main.npdm", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b[0x1000];   // Tropical Freeze's ACI0 starts at 0x430 (Returns HD's lower): 0x400 read TF as Returns HD
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 0x80 || memcmp(b, "META", 4)) return 0;
    uint32_t aci;
    memcpy(&aci, b + 0x70, 4);
    if ((size_t)aci + 0x18 > n || memcmp(b + aci, "ACI0", 4)) return 0;
    memcpy(pid, b + aci + 0x10, 8);
    return 1;
}

static void detect(void) {
    char exe[1024], dir[1200];
    uint64_t pid = 0;
    const char *e = getenv("MP4_EXEFS");   // main.c portable_paths' order: the variable, exefs/ beside us, the build's
    int got = e && *e && read_pid(e, &pid);
    if (!got && plat_exe_path(exe, sizeof exe) && strrchr(exe, '/')) {
        *strrchr(exe, '/') = 0;
        snprintf(dir, sizeof dir, "%s/exefs", exe);
        got = read_pid(dir, &pid);
    }
    if (!got) got = read_pid(rt_default_exefs, &pid);
    if (got && pid == PID_DKCTF) g_game = SETTINGS_GAME_DKCTF;
    const char *m = getenv("CB_MACHINE");
    char name[64] = "";
    if (m && *m) {
        g_machine = !strcmp(m, "deck-lcd") ? SETTINGS_DECK_LCD : !strcmp(m, "deck-oled") ? SETTINGS_DECK_OLED : SETTINGS_PC;
        return;
    }
    FILE *f = fopen("/sys/devices/virtual/dmi/id/product_name", "r");
    if (f) {
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
    }
    if (!strncmp(name, "Jupiter", 7)) g_machine = SETTINGS_DECK_LCD;     // Steam Deck LCD
    else if (!strncmp(name, "Galileo", 7)) g_machine = SETTINGS_DECK_OLED;   // Steam Deck OLED
}

unsigned settings_game(void) {
    pthread_once(&g_once, detect);
    return g_game;
}

int settings_machine(void) {
    pthread_once(&g_once, detect);
    return g_machine;
}

const char *settings_machine_name(int m) {
    return m == SETTINGS_DECK_LCD ? "deck-lcd" : m == SETTINGS_DECK_OLED ? "deck-oled" : "pc";
}

static void fill_texts(void) {
    for (int i = 0; i < N(rows); i++) rows[i].label = row_text[i][0], rows[i].desc = row_text[i][1];
}

const SettingRow *settings_row(int i) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, fill_texts);
    unsigned g = settings_game();
    for (int k = 0; k < N(rows); k++)
        if ((rows[k].games & g) && i-- == 0) return &rows[k];
    return NULL;
}

const SettingRow *settings_find(const char *key) {
    const SettingRow *r;
    for (int i = 0; (r = settings_row(i)); i++)
        if (!strcmp(r->key, key) || !strcasecmp(r->cfg, key)) return r;
    return NULL;
}

const char *settings_default(const SettingRow *r) {
    int m = settings_machine();
    const char *d = m == SETTINGS_DECK_LCD ? r->def_deck_lcd : m == SETTINGS_DECK_OLED ? r->def_deck_oled : NULL;
    return d ? d : r->def_pc;
}

const SettingOption *settings_option(const SettingRow *r, const char *v) {
    for (int i = 0; i < r->n_opts; i++)
        if (!strcmp(r->opts[i].value, v)) return &r->opts[i];
    return NULL;
}

// Only the rows with a machine default of their own; called from cfg_value on every read, so no lock after the first.
const char *settings_cfg_default(const char *name) {
    if (settings_machine() == SETTINGS_PC) return NULL;
    const char *k = strncasecmp(name, "MP4_", 4) ? name : name + 4;
    for (int i = 0; i < N(rows); i++) {
        const SettingRow *r = &rows[i];
        if (strcasecmp(r->cfg + 4, k) || (!r->def_deck_lcd && !r->def_deck_oled)) continue;
        const char *d = settings_default(r);
        if (r->kind == SETTING_RANGE) return d;
        const SettingOption *o = settings_option(r, d);
        return o && o->stored ? o->stored : NULL;
    }
    return NULL;
}

const SettingOption *settings_current(const SettingRow *r) {
    const char *now = cfg_value(r->cfg);
    for (int i = 0; i < r->n_opts; i++)
        if (r->opts[i].stored && !strcmp(r->opts[i].stored, now)) return &r->opts[i];
    return NULL;
}

const char *settings_value(const SettingRow *r, char *out, size_t cap) {
    if (r->kind == SETTING_RANGE) {
        snprintf(out, cap, "%g", cfg_double(r->cfg));
        return out;
    }
    const SettingOption *o = settings_current(r);
    snprintf(out, cap, "%s", o ? o->value : cfg_value(r->cfg));   // a value set by hand outside the list: as it is
    return out;
}

static const char *en(UiTextId id) { return ui_text_in("en", id); }

const char *settings_set(const SettingRow *r, const char *v) {
    static char why[512];
    if (cfg_from_env(r->cfg)) {
        snprintf(why, sizeof why, "%s is set by the environment variable %s", r->key, r->cfg);
        return why;
    }
    if (r->unavailable != NA) {
        snprintf(why, sizeof why, "%s is unavailable: %s", r->key, en(r->unavailable));
        return why;
    }
    if (!strcmp(v, "default")) {
        const char *k = r->cfg + 4;
        cfg_reset(&k, 1);
        return NULL;
    }
    if (r->kind == SETTING_RANGE) {
        char *end;
        double d = strtod(v, &end);
        if (end == v || *end || d < r->lo - 1e-6 || d > r->hi + 1e-6) {
            snprintf(why, sizeof why, "%s takes a number from %g to %g, not '%s'", r->key, r->lo, r->hi, v);
            return why;
        }
        cfg_set(r->cfg, v);
        return NULL;
    }
    const SettingOption *o = settings_option(r, v);
    if (!o) {
        int n = snprintf(why, sizeof why, "%s takes", r->key);
        for (int i = 0; i < r->n_opts && n < (int)sizeof why; i++)
            n += snprintf(why + n, sizeof why - n, "%s %s", i ? "," : "", r->opts[i].value);
        snprintf(why + (n < (int)sizeof why ? n : (int)sizeof why - 1), sizeof why - (n < (int)sizeof why ? n : (int)sizeof why - 1), ", not '%s'", v);
        return why;
    }
    if (o->unavailable != NA) {
        snprintf(why, sizeof why, "%s=%s is unavailable: %s", r->key, v, en(o->unavailable));
        return why;
    }
    if (!*o->stored) {
        const char *k = r->cfg + 4;
        cfg_reset(&k, 1);
    } else {
        cfg_set(r->cfg, o->stored);
    }
    return NULL;
}

// ---- the command line
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);   // UTF-8 passes through
    }
    fputc('"', f);
}

static void json_text(FILE *f, const char *lang, UiTextId id) {
    if (id == NA) fputs("null", f);
    else json_str(f, ui_text_in(lang, id));
}

static void dump_row(FILE *f, const SettingRow *r, const char *lang) {
    const CfgMeta *m = cfg_meta(r->cfg);
    char now[64];
    fputs("        {\"key\": ", f), json_str(f, r->key);
    fputs(", \"label\": ", f), json_text(f, lang, r->label);
    fputs(", \"short_label\": ", f), json_str(f, settings_short_label(r));
    fputs(", \"description\": ", f), json_text(f, lang, r->desc);
    fprintf(f, ", \"type\": \"%s\"", r->kind == SETTING_RANGE ? "range" : "choice");
    fputs(", \"value\": ", f), json_str(f, settings_value(r, now, sizeof now));
    fputs(", \"default\": ", f), json_str(f, settings_default(r));
    fprintf(f, ", \"restart\": %s", m && !m->live ? "true" : "false");
    fputs(", \"set_by_env\": ", f), fputs(cfg_from_env(r->cfg) ? "true" : "false", f);
    fputs(", \"unavailable\": ", f), json_text(f, lang, r->unavailable);
    fputs(", \"cfg\": ", f), json_str(f, r->cfg + 4);
    if (r->kind == SETTING_RANGE) {
        fprintf(f, ", \"min\": %g, \"max\": %g, \"step\": %g, \"unit\": ", r->lo, r->hi, r->step);
        json_str(f, r->unit);
    } else {
        fputs(", \"options\": [", f);
        for (int i = 0; i < r->n_opts; i++) {
            const SettingOption *o = &r->opts[i];
            fputs(i ? ", {\"value\": " : "{\"value\": ", f), json_str(f, o->value);
            fputs(", \"label\": ", f);
            if (o->text == SETTING_LITERAL) json_str(f, o->literal ? o->literal : o->value);
            else json_text(f, lang, o->text);
            fputs(", \"short_label\": ", f);
            if (settings_short_value(r, o)) json_str(f, settings_short_value(r, o));
            else fputs("null", f);
            fputs(", \"unavailable\": ", f), json_text(f, lang, o->unavailable);
            fputs("}", f);
        }
        fputs("]", f);
    }
    fputs("}", f);
}

static int dump(const char *lang) {
    FILE *f = stdout;
    unsigned g = settings_game();
    fputs("{\"format\": 1, \"version\": ", f), json_str(f, rt_port_version);
    fputs(", \"game\": ", f), json_str(f, g == SETTINGS_GAME_DKCTF ? "dkctf" : "dkcr-hd");
    fputs(", \"machine\": ", f), json_str(f, settings_machine_name(settings_machine()));
    fputs(", \"file\": ", f), json_str(f, cfg_file());
    fputs(", \"groups\": [\n", f);
    int ng = 0;
    for (size_t k = 0; k < sizeof groups / sizeof *groups; k++) {
        const SettingRow *r;
        int nr = 0;
        for (int i = 0; (r = settings_row(i)); i++) {
            if (strcmp(r->group, groups[k].id)) continue;
            if (!nr++) {
                fputs(ng++ ? ",\n    {\"id\": " : "    {\"id\": ", f), json_str(f, groups[k].id);
                fputs(", \"label\": ", f), json_text(f, lang, groups[k].label);
                fputs(", \"rows\": [\n", f);
            } else {
                fputs(",\n", f);
            }
            dump_row(f, r, lang);
        }
        if (nr) fputs("\n    ]}", f);
    }
    fputs("\n]}\n", f);
    return 0;
}

static int set_pairs(int n, char **pairs) {
    FILE *f = stdout;
    const char *errors[64];
    int ne = 0;
    if (!n) {
        fputs("{\"ok\": false, \"errors\": [\"--settings-set needs key=value pairs\"]}\n", f);
        return 2;
    }
    // check every pair first: none is stored unless all can be
    for (int i = 0; i < n && ne < 64; i++) {
        char key[128];
        const char *eq = strchr(pairs[i], '=');
        if (!eq || eq == pairs[i] || (size_t)(eq - pairs[i]) >= sizeof key) {
            errors[ne++] = strdup("a pair is not key=value");
            continue;
        }
        memcpy(key, pairs[i], (size_t)(eq - pairs[i]));
        key[eq - pairs[i]] = 0;
        const SettingRow *r = settings_find(key);
        char msg[256];
        if (!r) {
            snprintf(msg, sizeof msg, "no setting %s in this game", key);
            errors[ne++] = strdup(msg);
            continue;
        }
        // a dry run: settings_set stores, so the check works on a copy of the decision
        const char *v = eq + 1;
        if (cfg_from_env(r->cfg) || r->unavailable != NA) {
            errors[ne++] = strdup(settings_set(r, v));
        } else if (strcmp(v, "default")) {
            if (r->kind == SETTING_RANGE) {
                char *end;
                double d = strtod(v, &end);
                if (end == v || *end || d < r->lo - 1e-6 || d > r->hi + 1e-6) errors[ne++] = strdup(settings_set(r, v));
            } else {
                const SettingOption *o = settings_option(r, v);
                if (!o || o->unavailable != NA) errors[ne++] = strdup(settings_set(r, v));
            }
        }
    }
    if (ne) {
        fputs("{\"ok\": false, \"errors\": [", f);
        for (int i = 0; i < ne; i++) {
            if (i) fputs(", ", f);
            json_str(f, errors[i]);
        }
        fputs("]}\n", f);
        return 2;
    }
    int restart = 0;
    fputs("{\"ok\": true, \"changed\": [", f);
    for (int i = 0; i < n; i++) {
        char key[128];
        const char *eq = strchr(pairs[i], '=');
        memcpy(key, pairs[i], (size_t)(eq - pairs[i]));
        key[eq - pairs[i]] = 0;
        const SettingRow *r = settings_find(key);
        settings_set(r, eq + 1);
        const CfgMeta *m = cfg_meta(r->cfg);
        restart |= m && !m->live;
        char now[64];
        fputs(i ? ", {\"key\": " : "{\"key\": ", f), json_str(f, r->key);
        fputs(", \"value\": ", f), json_str(f, settings_value(r, now, sizeof now));
        fputs("}", f);
    }
    int ok = cfg_save();
    fprintf(f, "], \"restart\": %s, \"saved\": %s, \"file\": ", restart ? "true" : "false", ok ? "true" : "false");
    json_str(f, cfg_file());
    fputs("}\n", f);
    return ok ? 0 : 1;
}

int settings_cli(int argc, char **argv) {
    const char *lang = "en";
    int i = 1;
    const char *mode = NULL;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--settings-lang") && i + 1 < argc) lang = argv[++i];
        else if (!strcmp(argv[i], "--settings-dump") || !strcmp(argv[i], "--settings-set")) {
            mode = argv[i++];
            break;
        } else {
            fprintf(stderr, "settings: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    cfg_init();
    if (mode && !strcmp(mode, "--settings-dump")) return i == argc ? dump(lang) : 2;
    return set_pairs(argc - i, argv + i);
}
