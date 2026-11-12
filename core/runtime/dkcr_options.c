// Country-Box: the PC settings inside Donkey Kong Country Returns HD's own Options screen. The game's Options planks
// stay as they are; one more plank (PC_OPTIONS_LABEL) is a clone of the game's last plank, made at run time from the
// player's copy (nothing of the game ships with us), and it opens our pages, made the same way. Every row reads and
// writes the settings model (settings.c), the same as the overlay menu and the launcher.
//
// How the game builds its Options (measured from main's IL2CPP code; offsets main+, version 1.0.0): one class,
// OptionsUIController, serves the title, the pause menu and the island map. Its Initialize (main+0x294ff90) runs when
// the screen's scene loads; its m_optionsPanel (UINavPanel, +0x28) holds the planks as UIButton[] _buttons (+0xB0)
// whose clicks are UnityEvents of the prefab calling the controller (ActivateAudioPanel, ActivateControlsPanel,
// ActivateRumblePanel). A plank's label is a LocalizationTarget in its children: SetLocalizationKey(hash) shows the
// game's text, SetNonTranslationText(string) shows ours.
//
//   CB_NATIVE_OPTIONS=1   add the rows (off by default until they are proven)
//   CB_NATIVE_OPTIONS=log only log what the Options screen holds
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "settings.h"
#include "ui_text.h"

#define PC_OPTIONS_LABEL "PC OPTIONS"   // working label ("PC Options" or "Recomp options")
#define VIBRATION_LABEL "VIBRATION"     // the game's "HD RUMBLE" plank, renamed

// OptionsUIController
#define OFF_OPT_INITIALIZE 0x294ff90ull
#define OFF_OPT_ACT_OPTIONS 0x2950190ull          // OptionsUIController.ActivateOptionsPanel()
#define OFF_OPT_ACT_OPTIONS_FORCED 0x29501b0ull   // OptionsUIController.ActivateOptionsPanelForced()
#define OFF_ADAPTER_ACT_OPTIONS 0x298c490ull      // IslandHUDOptionsScreenUnityAdapter.ActivateOptionsPanel() (map)
#define ADAPTER_WORLD_MAP 0x68                    // its WorldMapUIController
#define WORLD_MAP_OPTIONS 0x90                    // WorldMapUIController.m_optionsController
#define OPT_PANELS 0x20          // UIPanelsController m_panelsController
#define OPT_OPTIONS_PANEL 0x28   // UINavPanel m_optionsPanel
#define OPT_AUDIO_PANEL 0x30
#define OPT_RUMBLE_PANEL 0x40
#define OPT_RUMBLE_ACTIVE 0x78   // LocalizationTarget m_rumbleActive: an instance gives the class
#define OPT_MUSIC_TEXT 0x58      // TextMeshProUGUI m_music
// UINavPanel / UIPanelBase / UIButton
#define NAV_BUTTONS 0xB0
#define PANEL_ACTIVE 0x62
#define PANEL_ACTIVATED_EVENT 0x20     // UIPanelBase.PanelActivatedEvent (UnityEvent)
#define PANEL_DEACTIVATED_EVENT 0x28
// LocalizationTarget
#define LT_TEXTS 0x20            // List<TMP_Text> m_textsToLocalize
#define LT_PREDEFINED_KEY 0x48
#define LT_CURRENT_KEY 0x58
// IL2CPP objects
#define OBJ_CLASS 0x0
#define CLASS_NAME 0x10
#define CLASS_BYVAL_ARG 0x20     // Il2CppClass.byval_arg: the Il2CppType a RuntimeTypeHandle holds
#define ARRAY_LEN 0x18
#define ARRAY_DATA 0x20
#define LIST_ITEMS 0x10
#define LIST_SIZE 0x18
#define STRING_LEN 0x10
#define STRING_CHARS 0x14

// Engine and game methods called from the host (rt_call; the last argument of each is its MethodInfo*, 0)
#define FN_INSTANTIATE_PARENT 0x40e1690ull   // Object.Instantiate(Object, Transform parent, bool worldSpace)
#define FN_SET_NAME 0x40e0980ull             // Object.set_name(string)
#define FN_GET_NAME 0x40e0840ull             // Object.get_name()
#define FN_GET_TRANSFORM 0x40d23d0ull        // Component.get_transform()
#define FN_GET_PARENT 0x40ea780ull           // Transform.get_parent()
#define FN_CHILD_COUNT 0x40ed590ull          // Transform.get_childCount()
#define FN_GET_CHILD 0x40edb20ull            // Transform.GetChild(int)
#define FN_GET_GAMEOBJECT 0x40d2440ull       // Component.get_gameObject()
#define FN_ACTIVE_SELF 0x40d8640ull          // GameObject.get_activeSelf()
#define FN_LOCAL_POS 0x40eabe0ull            // Transform.get_localPosition_Injected(out Vector3)
#define FN_LOCAL_SCALE 0x40ebad0ull          // Transform.get_localScale_Injected(out Vector3)
#define FN_ANCHORED_POS 0x40e9740ull         // RectTransform.get_anchoredPosition_Injected(out Vector2)
#define FN_SET_ANCHORED_POS 0x40e9830ull     // RectTransform.set_anchoredPosition_Injected(ref Vector2)
#define FN_IN_CHILDREN 0x40d2a80ull          // Component.GetComponentInChildren(Type, bool includeInactive)
#define FN_TYPE_FROM_HANDLE 0x3d1bf80ull     // Type.GetTypeFromHandle(RuntimeTypeHandle)
#define FN_ALLOC_STRING 0x3b79090ull         // String.FastAllocateString(int)
#define FN_TMP_GET_TEXT 0x40109b0ull         // TMP_Text.get_text()
#define FN_LT_SET_TEXT 0x27b7750ull          // LocalizationTarget.SetNonTranslationText(string)
#define FN_NAV_ADD_BUTTON 0x1ded410ull       // UINavPanel.AddButton(UIButton)
#define OFF_NAV_ACTIVATE_SRC 0x1defb80ull    // UINavPanel.Activate(UIInputSource)
#define OFF_NAV_ACTIVATE 0x1defcb0ull        // UINavPanel.Activate()
#define FN_BUTTON_SHOW 0x1dec4d0ull          // UIButton.AnimatedButtonShow()
#define FN_GET_ON_CLICK 0x41db8b0ull         // Button.get_onClick()
#define FN_SET_ON_CLICK 0x41db8c0ull         // Button.set_onClick(ButtonClickedEvent)
#define BUTTON_ON_SELECT 0x108               // UIButton.OnSelectAction (UnityEvent)
#define BUTTON_ON_CLICK_EARLY 0x110          // UIButton.OnClickBeforeAnimationAction
#define FN_BUTTON_RESET 0x1debb50ull         // UIButton.ResetState(Action)
#define FN_GET_COMPONENT 0x40d24b0ull        // Component.GetComponent(Type)
#define OFF_OPT_AUDIO_LEVEL 0x2950bd0ull     // OptionsUIController.ChangeAudioLevel(float)
#define OFF_OPT_AUDIO_HOLD 0x2950a80ull      // OptionsUIController.ChangeAudioLevelHold(float)
#define OFF_OPT_AUDIO_MUSIC 0x2951240ull     // OptionsUIController.SetAudioTypeMusic()
#define OFF_OPT_AUDIO_SFX 0x2951250ull       // OptionsUIController.SetAudioTypeSFX()
#define OFF_OPT_ACT_RUMBLE 0x2950500ull      // OptionsUIController.ActivateRumblePanel()
#define OFF_OPT_ACT_AUDIO 0x29501d0ull       // OptionsUIController.ActivateAudioPanel()

static uint64_t g_main;
static void store_ptr(uint64_t addr, uint64_t value);
static int g_mode;   // 0 off, 1 rows, 2 log only
static __thread int g_in_init;

static uint64_t call(Cpu *c, uint64_t off, int n, uint64_t a0, uint64_t a1, uint64_t a2) {
    return rt_call(c, g_main + off, n + 1, a0, a1, a2, 0ull);   // n managed arguments, then MethodInfo* = 0
}

static int ok_ptr(uint64_t p) { return p >= 0x1000 && p < (1ull << 39) && !(p & 7); }

static const char *class_name(uint64_t obj) {
    if (!ok_ptr(obj) || !ok_ptr(LD64(obj + OBJ_CLASS))) return "?";
    uint64_t n = LD64(LD64(obj + OBJ_CLASS) + CLASS_NAME);
    return n ? (const char *)(uintptr_t)n : "?";
}

// A System.String's text (ASCII) into out.
static const char *str_text(uint64_t s, char *out, size_t cap) {
    size_t k = 0;
    if (ok_ptr(s)) {
        uint32_t n = LD32(s + STRING_LEN);
        for (; k < n && k + 1 < cap; k++) {
            uint16_t ch = LD16(s + STRING_CHARS + 2 * k);
            out[k] = ch < 0x80 ? (char)ch : '?';
        }
    }
    out[k] = 0;
    return out;
}

// A System.String of UTF-8 text (code points of the Basic Multilingual Plane).
static uint64_t new_string(Cpu *c, const char *text) {
    uint16_t u[256];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < 256;) {
        unsigned cp = *p++;
        if (cp >= 0xE0 && p[0] && p[1]) cp = ((cp & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F), p += 2;
        else if (cp >= 0xC0 && p[0]) cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F), p += 1;
        u[n++] = (uint16_t)cp;
    }
    uint64_t s = call(c, FN_ALLOC_STRING, 1, n, 0, 0);
    if (!ok_ptr(s)) return 0;
    for (size_t i = 0; i < n; i++) ST16(s + STRING_CHARS + 2 * i, u[i]);
    return s;
}

static uint64_t g_lt_type;   // System.Type of LocalizationTarget

#define CLASS_PARENT 0x58      // Il2CppClass.parent

// System.Type of the class named name among instance's class and its bases; 0 if none.
static uint64_t type_named(Cpu *c, uint64_t instance, const char *name) {
    uint64_t k = ok_ptr(instance) ? LD64(instance + OBJ_CLASS) : 0;
    for (int depth = 0; ok_ptr(k) && depth < 16; depth++, k = LD64(k + CLASS_PARENT)) {
        uint64_t n = LD64(k + CLASS_NAME);
        if (n && !strcmp((const char *)(uintptr_t)n, name)) return call(c, FN_TYPE_FROM_HANDLE, 1, k + CLASS_BYVAL_ARG, 0, 0);
    }
    return 0;
}

// The LocalizationTarget among a component's children (the plank's label), 0 if none.
static uint64_t label_of(Cpu *c, uint64_t comp) {
    return g_lt_type && ok_ptr(comp) ? call(c, FN_IN_CHILDREN, 2, comp, g_lt_type, 1) : 0;
}

static void label_text(Cpu *c, uint64_t lt, char *out, size_t cap) {
    out[0] = 0;
    if (!ok_ptr(lt)) return;
    uint64_t list = LD64(lt + LT_TEXTS);
    if (!ok_ptr(list) || !LD32(list + LIST_SIZE)) return;
    uint64_t tmp = LD64(LD64(list + LIST_ITEMS) + ARRAY_DATA);
    if (ok_ptr(tmp)) str_text(call(c, FN_TMP_GET_TEXT, 0, tmp, 0, 0), out, cap);
}

// Our text on a label: the game's key is cleared first, so neither its Start nor a language change puts the game's
// text back.
// A CommonLocalizationTarget (CommonLocalizationTarget.Initialize, main+0x27b4090) shows its common text again when
// both keys are 0; with a current key and no predefined one it keeps what it shows.
#define OUR_KEY 0xCB0CB0CBu
static void set_label(Cpu *c, uint64_t lt, const char *text) {
    if (!ok_ptr(lt)) return;
    ST32(lt + LT_PREDEFINED_KEY, 0);
    ST32(lt + LT_CURRENT_KEY, strcmp(class_name(lt), "CommonLocalizationTarget") ? 0 : OUR_KEY);
    uint64_t s = new_string(c, text);
    if (s) call(c, FN_LT_SET_TEXT, 1, lt, s, 0);
}

// A Vector2/3 an engine method writes through its out argument: on the guest stack below sp.
static void vec_out(Cpu *c, uint64_t fn, uint64_t obj, float out[3]) {
    c->sp -= 64;
    uint64_t at = c->sp + 16;
    ST64(at, 0), ST32(at + 8, 0);
    call(c, fn, 1, obj, at, 0);
    for (int i = 0; i < 3; i++) {
        uint32_t u = LD32(at + 4 * i);
        memcpy(&out[i], &u, 4);
    }
    c->sp += 64;
}

static void set_anchored(Cpu *c, uint64_t rect, float x, float y) {
    c->sp -= 64;
    uint64_t at = c->sp + 16;
    uint32_t ux, uy;
    memcpy(&ux, &x, 4), memcpy(&uy, &y, 4);
    ST32(at, ux), ST32(at + 4, uy);
    call(c, FN_SET_ANCHORED_POS, 1, rect, at, 0);
    c->sp += 64;
}

static void describe_children(Cpu *c, uint64_t parent) {
    char name[128];
    int k = (int)call(c, FN_CHILD_COUNT, 0, parent, 0, 0);
    for (int j = 0; j < k && j < 12; j++) {
        uint64_t t = call(c, FN_GET_CHILD, 1, parent, (uint64_t)j, 0);
        float lp[3], sc[3], ap[3];
        vec_out(c, FN_LOCAL_POS, t, lp);
        vec_out(c, FN_LOCAL_SCALE, t, sc);
        vec_out(c, FN_ANCHORED_POS, t, ap);
        uint64_t go = call(c, FN_GET_GAMEOBJECT, 0, t, 0, 0);
        rt_log("options:   Buttons child %d '%s' %s active %d local (%.1f, %.1f) scale (%.2f, %.2f) anchored (%.1f, %.1f)", j,
               str_text(call(c, FN_GET_NAME, 0, t, 0, 0), name, sizeof name), class_name(t),
               (int)(call(c, FN_ACTIVE_SELF, 0, go, 0, 0) & 1), lp[0], lp[1], sc[0], sc[1], ap[0], ap[1]);
    }
}

static uint64_t g_tmp_type;   // System.Type of TMP_Text

// The hierarchy under t, with the labels and texts it holds (discovery; CB_NATIVE_OPTIONS=log).
static void dump_tree(Cpu *c, uint64_t t, int depth) {
    char name[128], text[128], tt[128] = "";
    uint64_t lt = call(c, FN_GET_COMPONENT, 1, t, g_lt_type, 0), tmp = call(c, FN_GET_COMPONENT, 1, t, g_tmp_type, 0);
    label_text(c, lt, text, sizeof text);
    if (ok_ptr(tmp)) str_text(call(c, FN_TMP_GET_TEXT, 0, tmp, 0, 0), tt, sizeof tt);
    float ap[3];
    vec_out(c, FN_ANCHORED_POS, t, ap);
    uint64_t go = call(c, FN_GET_GAMEOBJECT, 0, t, 0, 0);
    rt_log("options: %*s'%s' active %d at (%.0f, %.0f)%s%s%s%s%s%s", depth * 2, "", str_text(call(c, FN_GET_NAME, 0, t, 0, 0), name, sizeof name),
           (int)(call(c, FN_ACTIVE_SELF, 0, go, 0, 0) & 1), ap[0], ap[1], ok_ptr(lt) ? " label " : "", ok_ptr(lt) ? class_name(lt) : "",
           ok_ptr(lt) ? " " : "", ok_ptr(lt) ? text : "", ok_ptr(tmp) ? " text " : "", ok_ptr(tmp) ? tt : "");
    int k = depth < 5 ? (int)call(c, FN_CHILD_COUNT, 0, t, 0, 0) : 0;
    for (int j = 0; j < k && j < 16; j++) dump_tree(c, call(c, FN_GET_CHILD, 1, t, (uint64_t)j, 0), depth + 1);
}

static int hook_log_call(Cpu *c, void *arg) {
    float f;
    uint32_t u = (uint32_t)c->v[0];
    memcpy(&f, &u, 4);
    rt_log("options: call main+%#llx (s0 %.3f)", (unsigned long long)(uintptr_t)arg, f);
    return 0;
}

static void describe(Cpu *c, uint64_t ctrl) {
    uint64_t panel = LD64(ctrl + OPT_OPTIONS_PANEL);
    uint64_t buttons = ok_ptr(panel) ? LD64(panel + NAV_BUTTONS) : 0;
    uint32_t n = ok_ptr(buttons) ? LD32(buttons + ARRAY_LEN) : 0;
    char name[128], text[128];
    uint64_t parent = n ? call(c, FN_GET_PARENT, 0, call(c, FN_GET_TRANSFORM, 0, LD64(buttons + ARRAY_DATA), 0, 0), 0, 0) : 0;
    rt_log("options: controller %#llx (%s), options panel %#llx (%s), %u planks under %s (%d children)",
           (unsigned long long)ctrl, class_name(ctrl), (unsigned long long)panel, class_name(panel), n,
           parent ? str_text(call(c, FN_GET_NAME, 0, parent, 0, 0), name, sizeof name) : "-",
           parent ? (int)call(c, FN_CHILD_COUNT, 0, parent, 0, 0) : -1);
    if (parent) describe_children(c, parent);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t b = LD64(buttons + ARRAY_DATA + 8 * i), lt = label_of(c, b);
        label_text(c, lt, text, sizeof text);
        rt_log("options:   plank %u: %s '%s', label %s key %08x '%s'", i, class_name(b),
               str_text(call(c, FN_GET_NAME, 0, b, 0, 0), name, sizeof name), class_name(lt),
               ok_ptr(lt) ? LD32(lt + LT_PREDEFINED_KEY) : 0, text);
    }
}

// ---- our pages
#define FN_FIND 0x40ed750ull                 // Transform.Find(string path)
#define FN_SET_ACTIVE 0x40d85c0ull           // GameObject.SetActive(bool)
#define FN_TMP_SET_TEXT 0x4010b40ull         // TMP_Text.set_text(string)
#define FN_NAV_INITIALIZE 0x1ded720ull       // UINavPanel.Initialize()
#define FN_NAV_CURRENT 0x1ded6f0ull          // UINavPanel.get_CurrentSelected()
#define OFF_OPT_ACT_CONTROLS 0x29504e0ull    // OptionsUIController.ActivateControlsPanel()
#define OFF_OPT_ACT_PAIRING 0x2950720ull     // OptionsUIController.ActivatePairingPanel() (the Controls plank)

#define ROWS 4
#define CATS 4
// The categories and their rows (settings.c keys), at most ROWS each: a cloned Audio page holds four. Iteration 1,
// for review.
static const struct { const char *title; const char *keys[ROWS]; } cats[CATS] = {
    {"DISPLAY", {"video.display_mode", "video.refresh", "video.vsync", "video.perf_overlay"}},
    {"GRAPHICS", {"video.render_resolution", "video.upscaler", "video.sharpness", "video.aspect"}},
    {"CONTROLS", {"controls.pad_layout", "controls.rumble_strength", "controls.stick_deadzone", "controls.rumble_triggers"}},
    {"MORE", {"video.anisotropy", "sound.volume", "interface.menu_language", "controls.rumble_curve"}},
};

typedef struct {
    uint64_t ctrl, panels, opt_panel, plank;   // the game's controller, its panels controller and options panel; our plank
    uint64_t cat_panel, cat_extra;             // the categories page (a clone of the options panel) and its added plank
    uint64_t page, page_title, rows[ROWS], values[ROWS], extra[ROWS];   // one category's page (a clone of Audio)
    int cat, hold;
} Ctx;
static Ctx g_ctx[4];
static int g_nctx;

static Ctx *ctx_of(uint64_t ctrl) {
    for (int i = 0; i < g_nctx; i++)
        if (g_ctx[i].ctrl == ctrl) return &g_ctx[i];
    return NULL;
}

static uint64_t tr(Cpu *c, uint64_t comp) { return call(c, FN_GET_TRANSFORM, 0, comp, 0, 0); }

static uint64_t find(Cpu *c, uint64_t comp, const char *path) {
    uint64_t s = new_string(c, path);
    return s ? call(c, FN_FIND, 1, tr(c, comp), s, 0) : 0;
}

static uint64_t buttons_of(uint64_t panel) { return LD64(panel + NAV_BUTTONS); }
static uint32_t count_of(uint64_t panel) { return ok_ptr(buttons_of(panel)) ? LD32(buttons_of(panel) + ARRAY_LEN) : 0; }
static uint64_t button_at(uint64_t panel, uint32_t i) { return LD64(buttons_of(panel) + ARRAY_DATA + 8 * i); }

static void set_name(Cpu *c, uint64_t obj, const char *name) {
    uint64_t s = new_string(c, name);
    if (s) call(c, FN_SET_NAME, 1, obj, s, 0);
}

// A clone of button under its parent, one step (the distance of the last two buttons) below the last button of panel,
// added to the panel's navigation.
static uint64_t add_button(Cpu *c, uint64_t panel, uint64_t button, const char *name) {
    uint32_t n = count_of(panel);
    uint64_t last = button_at(panel, n - 1);
    uint64_t clone = call(c, FN_INSTANTIATE_PARENT, 2, button, call(c, FN_GET_PARENT, 0, tr(c, button), 0, 0), 0);
    if (!ok_ptr(clone)) return 0;
    // the planks have no layout group: each sits at its own anchored position, a fixed step apart
    float prev[3], at[3];
    vec_out(c, FN_ANCHORED_POS, tr(c, button_at(panel, n - 2)), prev);
    vec_out(c, FN_ANCHORED_POS, tr(c, last), at);
    set_anchored(c, tr(c, clone), at[0], at[1] + (at[1] - prev[1]));
    set_name(c, clone, name);
    call(c, FN_NAV_ADD_BUTTON, 1, panel, clone, 0);
    return clone;
}

// A clone of a whole panel beside it, initialized as the game initializes its own.
static uint64_t clone_panel(Cpu *c, uint64_t panel, const char *name) {
    uint64_t clone = call(c, FN_INSTANTIATE_PARENT, 2, panel, call(c, FN_GET_PARENT, 0, tr(c, panel), 0, 0), 0);
    if (!ok_ptr(clone)) return 0;
    set_name(c, clone, name);
    call(c, FN_NAV_INITIALIZE, 0, clone, 0, 0);
    return clone;
}

static const char *en(UiTextId id) { return ui_text_in("en", id); }

static void row_value(const SettingRow *r, char *out, size_t cap) {
    if (r->kind == SETTING_RANGE) {
        snprintf(out, cap, "%g%s", cfg_double(r->cfg), r->unit);
        return;
    }
    const SettingOption *o = settings_current(r);
    const char *sh = o ? settings_short_value(r, o) : NULL;
    if (sh) snprintf(out, cap, "%s", sh);
    else if (!o) snprintf(out, cap, "%s", cfg_value(r->cfg));
    else if (o->text == SETTING_LITERAL) snprintf(out, cap, "%s", o->value);   // literal labels use a sign the game's
                                                                                // font lacks (x for U+00D7)
    else snprintf(out, cap, "%s", en(o->text));
}

static void set_tmp(Cpu *c, uint64_t tmp, const char *text) {
    uint64_t s = ok_ptr(tmp) ? new_string(c, text) : 0;
    if (s) call(c, FN_TMP_SET_TEXT, 1, tmp, s, 0);
}

static void fill_page(Cpu *c, Ctx *x) {
    set_label(c, x->page_title, cats[x->cat].title);
    for (int i = 0; i < ROWS; i++) {
        const SettingRow *r = cats[x->cat].keys[i] ? settings_find(cats[x->cat].keys[i]) : NULL;
        char v[64];
        if (!r || !ok_ptr(x->rows[i])) continue;
        const char *test = i == 0 ? getenv("CB_NATIVE_TEST_LABEL") : NULL;   // a glyph test ("RESOLUCIÓN")
        set_label(c, label_of(c, x->rows[i]), test && *test ? test : settings_short_label(r));
        row_value(r, v, sizeof v);
        set_tmp(c, x->values[i], v);
    }
}

// The panel's independent button (UIPanelBase._independentButtons, +0x68) for an input action, 0 if none.
#define PANEL_INDEPENDENT 0x68
#define IND_ACTION 0x20              // IndependentButtonController.actionId (UIInputAction)
#define IND_ON_PRESSED 0x68          // UnityEvent OnPressed
#define IND_ON_PRESS_ANIM 0x70       // UnityEvent OnStartPressAnimation
#define UI_ACTION_CANCEL 5
static uint64_t independent(uint64_t panel, uint32_t action) {
    uint64_t arr = ok_ptr(panel) ? LD64(panel + PANEL_INDEPENDENT) : 0;
    uint32_t n = ok_ptr(arr) ? LD32(arr + ARRAY_LEN) : 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t b = LD64(arr + ARRAY_DATA + 8 * i);
        if (ok_ptr(b) && LD32(b + IND_ACTION) == action) return b;
    }
    return 0;
}

static void build(Cpu *c, Ctx *x) {
    // the categories page, from the options panel before our plank is added to it
    x->cat_panel = clone_panel(c, x->opt_panel, "CB_PcOptionsPanel");
    if (x->cat_panel) {
        set_label(c, label_of(c, find(c, x->cat_panel, "Options/OptionsTxt")), PC_OPTIONS_LABEL);
        // the options panel's activated / deactivated events close the whole Options screen: the categories take the
        // Audio page's instead (a page under the options panel)
        uint64_t audio = LD64(x->ctrl + OPT_AUDIO_PANEL);
        store_ptr(x->cat_panel + PANEL_ACTIVATED_EVENT, LD64(audio + PANEL_ACTIVATED_EVENT));
        store_ptr(x->cat_panel + PANEL_DEACTIVATED_EVENT, LD64(audio + PANEL_DEACTIVATED_EVENT));
        // its B (an independent button of the panel) closed the Options screen (the adapter's OnCancel): it goes back
        // one page as the Audio page's does
        uint64_t mine = independent(x->cat_panel, UI_ACTION_CANCEL), theirs = independent(audio, UI_ACTION_CANCEL);
        if (mine && theirs) {
            store_ptr(mine + IND_ON_PRESSED, LD64(theirs + IND_ON_PRESSED));
            store_ptr(mine + IND_ON_PRESS_ANIM, LD64(theirs + IND_ON_PRESS_ANIM));
        } else {
            rt_log("options: no B button to rewire (categories %#llx, audio %#llx)", (unsigned long long)mine,
                   (unsigned long long)theirs);
        }
        uint32_t n = count_of(x->cat_panel);
        x->cat_extra = n ? add_button(c, x->cat_panel, button_at(x->cat_panel, n - 1), "CB_Category") : 0;
        // every category plank clicks as the Audio plank (the Controls plank also starts the controller pairing)
        uint64_t first = button_at(x->cat_panel, 0), click = call(c, FN_GET_ON_CLICK, 0, first, 0, 0);
        for (uint32_t i = 0; i < CATS && i < count_of(x->cat_panel); i++) {
            uint64_t b = button_at(x->cat_panel, i);
            set_label(c, label_of(c, b), cats[i].title);
            if (i && ok_ptr(click)) {
                call(c, FN_SET_ON_CLICK, 1, b, click, 0);
                store_ptr(b + BUTTON_ON_SELECT, LD64(first + BUTTON_ON_SELECT));
                store_ptr(b + BUTTON_ON_CLICK_EARLY, LD64(first + BUTTON_ON_CLICK_EARLY));
            }
        }
    }
    // one category's page, from the Audio panel (its rows: label, value, arrows), with rows added to four
    x->page = clone_panel(c, LD64(x->ctrl + OPT_AUDIO_PANEL), "CB_PcOptionsPage");
    if (x->page) {
        x->page_title = label_of(c, find(c, x->page, "Options/OptionsTxt"));
        for (int i = (int)count_of(x->page); i < ROWS; i++) x->extra[i] = add_button(c, x->page, button_at(x->page, 0), "CB_Row");
        for (uint32_t i = 0; i < ROWS && i < count_of(x->page); i++) {
            x->rows[i] = button_at(x->page, i);
            uint64_t vt = find(c, x->rows[i], "ButtonGraphics/ValueTxt");
            x->values[i] = vt ? call(c, FN_GET_COMPONENT, 1, vt, g_tmp_type, 0) : 0;
        }
        fill_page(c, x);
    }
    // our plank on the game's options panel
    uint32_t n = count_of(x->opt_panel);
    x->plank = add_button(c, x->opt_panel, button_at(x->opt_panel, n - 1), "CB_PcOptions");
    set_label(c, label_of(c, x->plank), PC_OPTIONS_LABEL);
    rt_log("options: PC OPTIONS plank %#llx, categories %#llx (%u planks), page %#llx (%u rows)", (unsigned long long)x->plank,
           (unsigned long long)x->cat_panel, x->cat_panel ? count_of(x->cat_panel) : 0, (unsigned long long)x->page,
           x->page ? count_of(x->page) : 0);
}

static int active(uint64_t panel) { return ok_ptr(panel) && LD8(panel + PANEL_ACTIVE); }

static int selected(Cpu *c, uint64_t panel) {
    uint64_t b = call(c, FN_NAV_CURRENT, 0, panel, 0, 0);
    for (uint32_t i = 0; i < count_of(panel); i++)
        if (button_at(panel, i) == b) return (int)i;
    return -1;
}

// UIPanelsController.ActivatePanel opens only a panel of its m_ControlledPanelsList (List<UIPanelBase>, +0x28): ours
// goes in its spare capacity (the list's own Add is generic code the host cannot call without its method data).
#define PANELS_LIST 0x28
#define LIST_VERSION 0x1C
#define FN_ARRAY_NEW 0x3d33560ull           // Array.CreateInstance(Type, int)
#define FN_ARRAY_COPY 0x3d335f0ull          // Array.Copy(Array, Array, int)
// The incremental GC's write barrier, as the game's code inlines it (UIPanelsController.GetAndInitializeChildPanels,
// main+0x1df70a0): while main+0x570e4f0 is set, a pointer stored at addr marks its 4 KiB card, bit (addr >> 12) & 63
// of the word ((addr >> 18) & 0x7fff) of the table at main+0x55424e0.
#define GC_BARRIER_ON 0x570e4f0ull
#define GC_CARDS 0x55424e0ull
static void store_ptr(uint64_t addr, uint64_t value) {
    ST64(addr, value);
    if (LD32(g_main + GC_BARRIER_ON))
        __atomic_fetch_or((uint64_t *)(uintptr_t)(g_main + GC_CARDS + ((addr >> 18) & 0x7fff) * 8), 1ull << ((addr >> 12) & 63),
                          __ATOMIC_SEQ_CST);
}

static int register_panel(Cpu *c, uint64_t panels, uint64_t panel) {
    uint64_t list = ok_ptr(panels) ? LD64(panels + PANELS_LIST) : 0, items = ok_ptr(list) ? LD64(list + LIST_ITEMS) : 0;
    if (!items || !ok_ptr(panel)) return 0;
    uint32_t n = LD32(list + LIST_SIZE), cap = LD32(items + ARRAY_LEN);
    for (uint32_t i = 0; i < n; i++)
        if (LD64(items + ARRAY_DATA + 8 * i) == panel) return 1;
    if (n >= cap) {   // a larger array of the same element type, as List<T> grows its own
        uint64_t type = type_named(c, panel, "UIPanelBase");
        uint64_t grown = type ? call(c, FN_ARRAY_NEW, 2, type, cap + 8, 0) : 0;
        if (!ok_ptr(grown)) {
            rt_log("options: the panels list is full (%u) and could not grow", n);
            return 0;
        }
        call(c, FN_ARRAY_COPY, 3, items, grown, n);
        store_ptr(list + LIST_ITEMS, grown);
        items = grown;
    }
    store_ptr(items + ARRAY_DATA + 8 * n, panel);
    ST32(list + LIST_SIZE, n + 1);
    ST32(list + LIST_VERSION, LD32(list + LIST_VERSION) + 1);
    return 1;
}


// One step of the selected row by dir (+1 / -1), saved at once.
static void step_row(Cpu *c, Ctx *x, int dir) {
    int i = selected(c, x->page);
    const SettingRow *r = i >= 0 && i < ROWS && cats[x->cat].keys[i] ? settings_find(cats[x->cat].keys[i]) : NULL;
    if (!r || r->unavailable != SETTING_LITERAL) return;
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
    set_tmp(c, x->values[i], v);
    rt_log("options: %s = %s", r->key, v);
}

// The game's handler of a plank, run with one of the controller's panel fields pointing at our panel: the game opens it
// on its own path (its timing, its input hand-over), then the field gets the game's panel back.
static void open_as(Cpu *c, Ctx *x, uint64_t handler, uint64_t field, uint64_t panel) {
    if (!register_panel(c, x->panels, panel)) return;
    uint64_t was = LD64(x->ctrl + field);
    store_ptr(x->ctrl + field, panel);
    g_in_init = 1;
    rt_call(c, g_main + handler, 2, x->ctrl, 0ull);
    g_in_init = 0;
    store_ptr(x->ctrl + field, was);
}

// The plank clicks: our plank on the game's options panel (a clone of its Vibration plank: ActivateRumblePanel) opens
// the categories; a category plank (all clones of the Audio plank: ActivateAudioPanel) opens the category's page.
// A hook that calls the guest and then lets the game's body run gives it back the registers it was entered with.
typedef struct { uint64_t x[31], sp; __uint128_t v[32]; } Regs;
static void regs_save(const Cpu *c, Regs *r) {
    memcpy(r->x, c->x, sizeof r->x), memcpy(r->v, c->v, sizeof r->v);
    r->sp = c->sp;
}
static int regs_back(Cpu *c, const Regs *r) {
    memcpy(c->x, r->x, sizeof r->x), memcpy(c->v, r->v, sizeof r->v);
    c->sp = r->sp;
    return 0;
}

static int hook_plank(Cpu *c, void *arg) {
    if (g_in_init) return 0;
    Ctx *x = ctx_of(c->x[0]);
    uint64_t off = (uint64_t)(uintptr_t)arg;
    int i;
    if (!x) return 0;
    Regs saved;
    regs_save(c, &saved);
    if (active(x->cat_panel) && (i = selected(c, x->cat_panel)) >= 0 && i < CATS) {
        x->cat = i;
        fill_page(c, x);
        open_as(c, x, OFF_OPT_ACT_AUDIO, OPT_AUDIO_PANEL, x->page);
    } else if (active(x->opt_panel) && call(c, FN_NAV_CURRENT, 0, x->opt_panel, 0, 0) == x->plank) {
        open_as(c, x, OFF_OPT_ACT_RUMBLE, OPT_RUMBLE_PANEL, x->cat_panel);
    } else {
        return regs_back(c, &saved);
    }
    rt_log("options: plank main+%#llx opened %s", (unsigned long long)off, active(x->page) ? "a page" : "our panel");
    c->x[0] = 0;
    return 1;
}

// Left and right on a row of our page: the arrows call the game's audio handlers.
static int hook_audio(Cpu *c, void *arg) {
    Ctx *x = ctx_of(c->x[0]);
    if (!x || !active(x->page)) return 0;
    uint64_t off = (uint64_t)(uintptr_t)arg;
    float f;
    uint32_t u = (uint32_t)c->v[0];
    memcpy(&f, &u, 4);
    if (off == OFF_OPT_AUDIO_LEVEL) {
        x->hold = 0;
        step_row(c, x, f < 0 ? -1 : 1);
    } else if (off == OFF_OPT_AUDIO_HOLD && ++x->hold > 24 && x->hold % 6 == 0) {   // held: repeat after 0.4 s
        step_row(c, x, f < 0 ? -1 : 1);
    }
    c->x[0] = 0;
    return 1;   // SetAudioTypeMusic / SFX and the game's audio levels are left alone on our page
}

// UINavPanel.Activate (both overloads): after the game's body, the buttons we added are shown as the panel shows its own.
static int hook_nav_activate(Cpu *c, void *arg) {
    if (g_in_init) return 0;
    uint64_t self = c->x[0], show[ROWS + 1] = {0};
    int n = 0;
    for (int k = 0; k < g_nctx && !n; k++) {
        Ctx *x = &g_ctx[k];
        if (self == x->opt_panel) show[n++] = x->plank;
        else if (self == x->cat_panel) show[n++] = x->cat_extra;
        else if (self == x->page)
            for (int i = 0; i < ROWS; i++)
                if (x->extra[i]) show[n++] = x->extra[i];
    }
    if (!n) return 0;
    g_in_init = 1;
    rt_call(c, g_main + (uint64_t)(uintptr_t)arg, 3, self, c->x[1], c->x[2]);
    g_in_init = 0;
    for (int i = 0; i < n; i++)
        if (show[i]) {
            call(c, FN_BUTTON_RESET, 1, show[i], 0, 0);
            call(c, FN_BUTTON_SHOW, 0, show[i], 0, 0);
        }
    c->x[0] = 0;
    return 1;
}

// The categories' B runs the Audio page's back event, whose target is the game's Audio page (outside the clone): a
// call on the game's Audio page while our categories are up is meant for them.
#define OFF_NAV_DEACTIVATE_PANEL 0x1df2900ull   // UINavPanel.DeactivatePanel()
#define OFF_NAV_DEACTIVATE 0x1df1400ull         // UINavPanel.Deactivate()
#define OFF_PANELS_DEACTIVATE 0x1df7720ull      // UIPanelsController.DeactivatePanel(bool, int)
static int hook_back(Cpu *c, void *arg) {
    (void)arg;
    for (int k = 0; k < g_nctx; k++) {
        Ctx *x = &g_ctx[k];
        if (c->x[0] == LD64(x->ctrl + OPT_AUDIO_PANEL) && active(x->cat_panel) && !active(c->x[0])) {
            c->x[0] = x->cat_panel;
        }
    }
    return 0;
}

// Our plank and pages for one controller (once), and the Vibration words.
static void setup(Cpu *c, uint64_t ctrl) {
    uint64_t panel = LD64(ctrl + OPT_OPTIONS_PANEL);
    if (!g_lt_type || !g_tmp_type || ctx_of(ctrl) || !ok_ptr(panel) || count_of(panel) < 2) return;
    Ctx *x = &g_ctx[g_nctx < 4 ? g_nctx++ : (g_nctx = 4, 3)];   // the newest replaces the oldest kept past four
    memset(x, 0, sizeof *x);
    x->ctrl = ctrl, x->panels = LD64(ctrl + OPT_PANELS), x->opt_panel = panel;
    // the game's "HD Rumble" plank and page title say Vibration: words only
    set_label(c, label_of(c, button_at(panel, count_of(panel) - 1)), VIBRATION_LABEL);
    set_label(c, label_of(c, find(c, LD64(ctrl + OPT_RUMBLE_PANEL), "Options/OptionsTxt")), VIBRATION_LABEL);
    build(c, x);
}

// OptionsUIController.Initialize: the game's body first (rt_call, with the hook passed over), then ours.
static int hook_initialize(Cpu *c, void *arg) {
    (void)arg;
    if (g_in_init) return 0;
    uint64_t ctrl = c->x[0];
    g_in_init = 1;
    rt_call(c, g_main + OFF_OPT_INITIALIZE, 2, ctrl, c->x[1]);
    g_in_init = 0;
    uint64_t lt = LD64(ctrl + OPT_RUMBLE_ACTIVE);
    if (!g_lt_type) g_lt_type = type_named(c, lt, "LocalizationTarget");   // m_rumbleActive: a CommonLocalizationTarget
    if (!g_tmp_type) g_tmp_type = type_named(c, LD64(ctrl + OPT_MUSIC_TEXT), "TMP_Text");
    if (g_mode == 2) describe(c, ctrl);
    if (g_mode == 2 && g_lt_type && g_tmp_type) {
        rt_log("options: audio panel:");
        dump_tree(c, call(c, FN_GET_TRANSFORM, 0, LD64(ctrl + OPT_AUDIO_PANEL), 0, 0), 0);
        rt_log("options: rumble panel:");
        dump_tree(c, call(c, FN_GET_TRANSFORM, 0, LD64(ctrl + OPT_RUMBLE_PANEL), 0, 0), 0);
        rt_log("options: options panel:");
        dump_tree(c, call(c, FN_GET_TRANSFORM, 0, LD64(ctrl + OPT_OPTIONS_PANEL), 0, 0), 0);
    }
    if (g_mode == 1) setup(c, ctrl);
    c->x[0] = 0;
    return 1;
}

// OptionsUIController.ActivateOptionsPanel(Forced): the island map's controller opens its Options without the
// Initialize the title and the pause menu run first; ours are built then.
static int hook_activate_options(Cpu *c, void *arg) {
    uint64_t ctrl = c->x[0];
    if (arg) {   // IslandHUDOptionsScreenUnityAdapter: its WorldMapUIController's (+0x68) OptionsController (+0x90)
        uint64_t map = LD64(c->x[0] + ADAPTER_WORLD_MAP);
        ctrl = ok_ptr(map) ? LD64(map + WORLD_MAP_OPTIONS) : 0;
        if (!ok_ptr(ctrl) || strcmp(class_name(ctrl), "OptionsUIController")) return 0;
    }
    if (ctx_of(ctrl)) return 0;
    Regs saved;
    regs_save(c, &saved);
    if (!g_lt_type) g_lt_type = type_named(c, LD64(ctrl + OPT_RUMBLE_ACTIVE), "LocalizationTarget");
    if (!g_tmp_type) g_tmp_type = type_named(c, LD64(ctrl + OPT_MUSIC_TEXT), "TMP_Text");
    setup(c, ctrl);
    return regs_back(c, &saved);
}

// CB_NATIVE_OPTIONS_TRACE=1: our panels' state and the panel stack twice a second (present thread; memory reads only).
void dkcr_options_frame(uint64_t frame) {
    static int trace = -1;
    if (trace < 0) trace = getenv("CB_NATIVE_OPTIONS_TRACE") != NULL;
    if (g_mode != 1 || !trace || frame % 30) return;
    for (int k = 0; k < g_nctx; k++) {
        Ctx *x = &g_ctx[k];
        uint64_t st = LD64(x->panels + 0x30), arr = ok_ptr(st) ? LD64(st + 0x10) : 0;
        uint32_t n = ok_ptr(st) ? LD32(st + 0x18) : 0;
        char line[256];
        int m = snprintf(line, sizeof line, "stack %u:", n);
        for (uint32_t i = 0; i < n && i < 6 && m < (int)sizeof line; i++) {
            uint64_t p = LD64(arr + 0x20 + 8 * i);
            m += snprintf(line + m, sizeof line - m, " %s", p == x->opt_panel ? "options" : p == x->cat_panel ? "categories"
                                                           : p == x->page ? "page" : p == LD64(x->panels + 0x20) ? "root" : "other");
        }
        if (active(x->opt_panel) || active(x->cat_panel) || active(x->page))
            rt_log("options: frame %llu: active options %d categories %d page %d; root is options %d; %s", (unsigned long long)frame,
                   active(x->opt_panel), active(x->cat_panel), active(x->page), LD64(x->panels + 0x20) == x->opt_panel, line);
    }
}

void dkcr_options_init(uint64_t main_base) {
    const char *e = getenv("CB_NATIVE_OPTIONS");
    g_mode = !e ? 0 : !strcmp(e, "1") ? 1 : !strcmp(e, "log") ? 2 : 0;
    if (!g_mode || settings_game() != SETTINGS_GAME_DKCR) return;
    g_main = main_base;
    rt_entry_hook_set(g_main + OFF_OPT_INITIALIZE, hook_initialize, NULL);
    if (g_mode == 2) {
        static const uint64_t logged[] = {OFF_OPT_AUDIO_LEVEL, OFF_OPT_AUDIO_HOLD, OFF_OPT_AUDIO_MUSIC, OFF_OPT_AUDIO_SFX,
                                          OFF_OPT_ACT_RUMBLE, OFF_OPT_ACT_AUDIO};
        for (size_t i = 0; i < sizeof logged / sizeof *logged; i++)
            rt_entry_hook_set(g_main + logged[i], hook_log_call, (void *)(uintptr_t)logged[i]);
    }
    if (g_mode == 1) {
        rt_entry_hook_set(g_main + OFF_OPT_ACT_OPTIONS, hook_activate_options, NULL);
        rt_entry_hook_set(g_main + OFF_OPT_ACT_OPTIONS_FORCED, hook_activate_options, NULL);
        rt_entry_hook_set(g_main + OFF_ADAPTER_ACT_OPTIONS, hook_activate_options, (void *)1);
        rt_entry_hook_set(g_main + OFF_NAV_ACTIVATE_SRC, hook_nav_activate, (void *)(uintptr_t)OFF_NAV_ACTIVATE_SRC);
        rt_entry_hook_set(g_main + OFF_NAV_ACTIVATE, hook_nav_activate, (void *)(uintptr_t)OFF_NAV_ACTIVATE);
        static const uint64_t planks[] = {OFF_OPT_ACT_AUDIO, OFF_OPT_ACT_CONTROLS, OFF_OPT_ACT_PAIRING, OFF_OPT_ACT_RUMBLE};
        for (size_t i = 0; i < sizeof planks / sizeof *planks; i++)
            rt_entry_hook_set(g_main + planks[i], hook_plank, (void *)(uintptr_t)planks[i]);
        static const uint64_t back[] = {OFF_NAV_DEACTIVATE_PANEL, OFF_NAV_DEACTIVATE, OFF_PANELS_DEACTIVATE};
        for (size_t i = 0; i < sizeof back / sizeof *back; i++)
            rt_entry_hook_set(g_main + back[i], hook_back, (void *)(uintptr_t)back[i]);
        static const uint64_t audio[] = {OFF_OPT_AUDIO_LEVEL, OFF_OPT_AUDIO_HOLD, OFF_OPT_AUDIO_MUSIC, OFF_OPT_AUDIO_SFX};
        for (size_t i = 0; i < sizeof audio / sizeof *audio; i++)
            rt_entry_hook_set(g_main + audio[i], hook_audio, (void *)(uintptr_t)audio[i]);
    }
    rt_log("options: native PC options %s", g_mode == 1 ? "on" : "logging only");
}
