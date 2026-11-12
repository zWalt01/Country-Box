// From fe3h-pc-recomp runtime/swkbd_win.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only.
// From totk-pc-recomp runtime/swkbd_win.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: renamed from swkbd_win.c; the prompt is the overlay's (ImGui) with SDL3 text input instead of a Win32 window (Linux).
// Adapted from MacNativeGames games/totk/runtime/swkbd_host.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors): the ShowKeyboardArg offsets, the UTF-8/UTF-16 conversions, the disabled-key filter and the result codes.
// The macOS text bar is replaced by a prompt drawn by the in-game overlay (ui/menu.h text entry).
//
// nn::swkbd::ShowKeyboard: text entry for the game. On the Switch the call starts the keyboard library applet, which
// takes the screen until the player confirms or cancels. TotK calls it in one place, naming a horse at a stable
// (main+0x1ecbf64 -> main+0x1b2b3a0: UserName preset, at most 9 characters, '@' '%' '\' disabled, UTF-16 out, no
// header, guide or initial text; an empty answer counts as not named).
//
// Front end: a prompt the overlay draws over the game (ui/menu.cpp layout_entry) while the guest thread that called
// waits (the call blocks, as on the Switch). The thread gives its virtual core up meanwhile, so the other guest threads
// keep running and the game keeps presenting, which is what draws the prompt; if no present draws it for 3 s the
// entry counts as cancelled. It holds no lock across the wait but `one`. The text starts as the game's initial text,
// else a default name from the catalog (ui_text.c), selected, so a controller can confirm it: A = OK, B = cancel (each
// after it was seen released, read from the gamepad alone so typed keys bound to buttons do not confirm); Enter / Esc
// and the buttons work too. SDL text input is on meanwhile: typed text, IME, and on the Steam Deck the Steam on-screen
// keyboard. While it is open, and after it closes until everything is released, the game reads no input
// (input_block). text_entry_run is the only front-end seam.
//
// Environment:
//   MP4_SWKBD_ANSWER=<text>|<cancel>|<initial>   answer without a window (probes, scripted runs); <initial> keeps the
//                                                 starting text (the game's, else the default name)
//   MP4_SWKBD_TEST=ok|cancel                     open the window, then press OK / Cancel on the first timer tick
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"
#include "vk.h"

// KeyboardConfig then the work buffer {pointer, size}: offsets read from the SDK setters (MNG) and checked against
// the SDK by --probe-services. SDK 20.5 (1.4.3) put a text-check callback at +0x4c8 and grew KeyboardConfig from
// 0x4d0 to 0x518 bytes (SetInitialText, SetUserWordList read the work buffer there); the fields below +0x3d8 stay.
#define ARG_WORK (RT_V143 ? 0x518 : 0x4d0)
#define ARG_WORK_SIZE (ARG_WORK + 8)
enum {
    KB_TYPE = 0x00, KB_OK_TEXT = 0x04, KB_DISABLED = 0x1c, KB_HEADER = 0x24, KB_SUB = 0xa6, KB_GUIDE = 0x1a8,
    KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0, KB_PASSWORD = 0x3b4, KB_UTF8 = 0x3bd, KB_INIT_OFF = 0x3c0, KB_INIT_LEN = 0x3c4,
    KB_TYPE_NUMPAD = 1, KB_LIMIT = 500, SWKBD_CANCELED = 0x29f,
};
enum { B_A = 0, B_B = 1, B_PLUS = 10 };   // input.h bits

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static size_t u16_len(const uint16_t *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

// UTF-16 (n units) -> NUL-terminated UTF-8 in cap bytes, cut at a character boundary. Returns the bytes written.
static size_t utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        char b[4];
        size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
    return o;
}

// UTF-8 -> UTF-16, at most cap units (a character that does not fit whole is dropped); bad bytes become U+FFFD.
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        uint32_t c;
        int k;
        if (*p < 0x80) { c = *p; k = 0; }
        else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; k = 1; }
        else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; k = 2; }
        else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; k = 3; }
        else { c = 0xfffd; k = 0; }
        p++;
        for (int i = 0; i < k; i++, p++) {
            if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; }
            c = c << 6 | (*p & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 > cap) break;
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            if (n + 1 > cap) break;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

// Whether the applet would let this character through (single UTF-16 units; pairs are always allowed). Disabled-key
// bits as the applet's: 1 space, 2 '@', 3 '%', 4 '/', 5 '\', 6 digits.
static int allowed(uint16_t c, uint32_t disabled, int digits_only) {
    int digit = c >= '0' && c <= '9';
    if (digits_only && !digit) return 0;
    if (c < 0x20 || c == 0x7f) return 0;
    if ((disabled & 1u << 1 && c == ' ') || (disabled & 1u << 2 && c == '@') || (disabled & 1u << 3 && c == '%')) return 0;
    if ((disabled & 1u << 4 && c == '/') || (disabled & 1u << 5 && c == '\\') || (disabled & 1u << 6 && digit)) return 0;
    return 1;
}

// Drop disallowed characters and keep at most max units without splitting a surrogate pair.
static size_t clean(uint16_t *t, size_t n, size_t max, uint32_t disabled, int digits_only) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int pair = t[i] >= 0xd800 && t[i] < 0xdc00 && i + 1 < n;
        size_t k = pair ? 2 : 1;
        if (!pair && !allowed(t[i], disabled, digits_only)) continue;
        if (o + k > max) break;
        t[o++] = t[i];
        if (pair) t[o++] = t[++i];
    }
    return o;
}

// ---- the request, front-end independent
typedef struct {
    uint16_t prompt[0x101];              // header, else guide, else sub text (UTF-16, NUL-terminated; may be empty)
    uint16_t initial[KB_LIMIT + 1];      // starting text (the game's, else the catalog's default name)
    int game_initial;                    // the starting text is the game's
    int max_len, min_len, password, numeric;
    uint32_t disabled;
} SwkbdRequest;

// ---- front end: the overlay's prompt (menu.h text entry) over the game window, with SDL text input on (on the Steam
// Deck SDL asks Steam for its on-screen keyboard). The guest thread that called waits here, polling every 16 ms.
#define TICK_NS 16000000ull
#define NO_FRAME_NS 3000000000ull   // the overlay drew nothing this long (no presents): the entry gives up as cancelled

static const SwkbdRequest *g_req;   // under `one` (ov_show_keyboard): the request allow() filters for

static int allow_typed(uint32_t c) {
    if (c >= 0x10000) return 1;   // a pair: always allowed, as in clean()
    return allowed((uint16_t)c, g_req->disabled, g_req->numeric);
}

// Shows the prompt and waits for the answer. Returns 1 with the text in out (n units), 0 when cancelled.
static int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    int test = 0;   // MP4_SWKBD_TEST: 1 press OK, -1 press Cancel
    const char *t = cfg_get("MP4_SWKBD_TEST");
    if (t && *t) test = !strcmp(t, "cancel") ? -1 : 1;
    static char prompt[0x101 * 3 + 1], initial[KB_LIMIT * 3 + 1], answer[KB_LIMIT * 4 * 2 + 1];
    if (req->prompt[0]) utf16_to_utf8(req->prompt, u16_len(req->prompt, 0x100), prompt, sizeof prompt);
    else snprintf(prompt, sizeof prompt, "%s", ui_text(UI_SWKBD_PROMPT));
    utf16_to_utf8(req->initial, u16_len(req->initial, KB_LIMIT), initial, sizeof initial);
    g_req = req;
    MenuTextEntry me = {.title = ui_text(UI_SWKBD_TITLE), .prompt = prompt, .hint = ui_text(UI_SWKBD_PAD_HINT),
                        .ok = ui_text(UI_SWKBD_OK), .cancel = ui_text(UI_SWKBD_CANCEL), .initial = initial,
                        .max_units = req->max_len, .password = req->password, .allow = allow_typed};
    if (!gfx_window_handle() || !menu_text_entry_begin(&me)) {
        rt_log("swkbd: no window or overlay to show the text entry: cancelled");
        return 0;
    }
    gfx_window_text_input(1);
    uint64_t armed = 0, start = kern_now_ns();   // armed: buttons seen released since the entry opened
    int result = 0;
    for (int tick = 0; !result; tick++) {
        struct timespec ts = {0, (long)TICK_NS};
        nanosleep(&ts, NULL);
        int force = 0;
        if (test && tick) force = test;
        uint64_t b;
        int32_t lx, ly;
        if (!force && input_pad_read(&b, &lx, &ly)) {   // the gamepad alone: typed keys must not confirm
            uint64_t pressed = b & armed;   // down now, up at the previous tick
            armed = ~b;
            if (pressed & (1ull << B_A | 1ull << B_PLUS)) force = 1;
            else if (pressed & 1ull << B_B) force = -1;
        } else if (!force) {
            armed = ~0ull;
        }
        result = menu_text_entry_poll(force, answer, sizeof answer);
        if (!result && !menu_text_entry_frames() && kern_now_ns() - start > NO_FRAME_NS) {
            rt_log("swkbd: no present drew the text entry in %.0f s: cancelled", NO_FRAME_NS / 1e9);
            result = menu_text_entry_poll(-1, NULL, 0);
        }
    }
    gfx_window_text_input(0);
    menu_text_entry_end();
    if (result < 0) return 0;
    *n = utf8_to_utf16(answer, out, cap);
    return 1;
}

// ---- nn::swkbd::ShowKeyboard
static void request_from(const uint8_t *arg, SwkbdRequest *r) {
    memset(r, 0, sizeof *r);
    int32_t max_len = (int32_t)rd32(arg + KB_MAX_LEN), min_len = (int32_t)rd32(arg + KB_MIN_LEN);
    r->max_len = max_len <= 0 || max_len > KB_LIMIT ? KB_LIMIT : max_len;
    r->min_len = min_len < 0 ? 0 : min_len > r->max_len ? r->max_len : min_len;
    r->password = rd32(arg + KB_PASSWORD) != 0;
    r->numeric = rd32(arg + KB_TYPE) == KB_TYPE_NUMPAD;
    r->disabled = rd32(arg + KB_DISABLED);
    static const struct { int off; size_t max; } texts[] = {{KB_HEADER, 0x40}, {KB_GUIDE, 0x100}, {KB_SUB, 0x80}};
    for (size_t i = 0; i < 3 && !r->prompt[0]; i++) {
        const uint16_t *t = (const uint16_t *)(arg + texts[i].off);
        size_t n = u16_len(t, texts[i].max);
        memcpy(r->prompt, t, n * 2);
        r->prompt[n] = 0;
    }
    uint64_t work = rd64(arg + ARG_WORK), work_size = rd64(arg + ARG_WORK_SIZE);
    int32_t init_off = (int32_t)rd32(arg + KB_INIT_OFF), init_len = (int32_t)rd32(arg + KB_INIT_LEN);
    size_t n = 0;
    if (init_len > 0 && work && init_off >= 0 && (uint64_t)init_off + (uint64_t)init_len * 2 <= work_size) {
        const uint16_t *t = (const uint16_t *)(uintptr_t)(work + (uint64_t)init_off);
        n = u16_len(t, (size_t)(init_len > KB_LIMIT ? KB_LIMIT : init_len));
        memcpy(r->initial, t, n * 2);
        r->game_initial = n > 0;
    }
    if (!n) n = utf8_to_utf16(ui_text(UI_SWKBD_DEFAULT_NAME), r->initial, KB_LIMIT);
    n = clean(r->initial, n, (size_t)r->max_len, r->disabled, r->numeric);
    r->initial[n] = 0;
}

// Result ShowKeyboard(String *out, const ShowKeyboardArg &arg); String = {pointer, byte size}
static void ov_show_keyboard(Cpu *c) {
    uint64_t *str = arg_p(c, 0);
    const uint8_t *arg = arg_p(c, 1);
    char lr[96];
    if (!str || !arg) rt_fatal("nn::swkbd::ShowKeyboard from %s: null argument", ldr_describe(c->x[30] - 4, lr, sizeof lr));
    static SwkbdRequest req;   // one keyboard at a time (the applet is modal too)
    static PlatMutex one = PLAT_MUTEX_INIT;
    int utf8 = arg[KB_UTF8] != 0;
    uint16_t u[KB_LIMIT * 2 + 1];
    size_t n = 0;
    int confirmed;
    const char *forced = cfg_get("MP4_SWKBD_ANSWER");
    int away = kern_core_away();
    plat_mutex_lock(&one);
    request_from(arg, &req);
    if (forced && *forced) {
        confirmed = strcmp(forced, "<cancel>") != 0;
        if (!strcmp(forced, "<initial>")) n = u16_len(req.initial, KB_LIMIT), memcpy(u, req.initial, n * 2);
        else if (confirmed) n = utf8_to_utf16(forced, u, KB_LIMIT * 2);
    } else {
        input_block(1);
        confirmed = text_entry_run(&req, u, KB_LIMIT * 2, &n);
        input_block(0);
    }
    n = confirmed ? clean(u, n, (size_t)req.max_len, req.disabled, req.numeric) : 0;
    if (confirmed && (int)n < req.min_len) confirmed = 0, n = 0;
    uint64_t buf = str[0], size = str[1];
    if (buf && size) {
        if (utf8) utf16_to_utf8(u, n, (char *)(uintptr_t)buf, size);
        else {
            size_t units = size / 2;   // one kept for the terminator
            if (units) {
                if (n > units - 1) { n = units - 1; if (n && u[n - 1] >= 0xd800 && u[n - 1] < 0xdc00) n--; }
                memcpy((void *)(uintptr_t)buf, u, n * 2);
                ((uint16_t *)(uintptr_t)buf)[n] = 0;
            }
        }
    }
    char prompt[0x101 * 3 + 1], shown[KB_LIMIT * 3 + 1];
    utf16_to_utf8(req.prompt, u16_len(req.prompt, 0x100), prompt, sizeof prompt);
    utf16_to_utf8(u, n, shown, sizeof shown);
    if (req.password) snprintf(shown, sizeof shown, "(password, %zu units, not logged)", n);
    rt_log("swkbd: from %s: \"%s\" (%d-%d units, disabled keys %#x, %s out, %s start)%s -> %s%s%s",
           ldr_describe(c->x[30] - 4, lr, sizeof lr), prompt, req.min_len, req.max_len, req.disabled,
           utf8 ? "UTF-8" : "UTF-16", req.game_initial ? "game" : "default", forced && *forced ? " [MP4_SWKBD_ANSWER]" : "",
           confirmed ? "\"" : "cancelled", confirmed ? shown : "", confirmed ? "\"" : "");
    plat_mutex_unlock(&one);
    kern_core_back(&away);
    ret_x(c, confirmed ? 0 : SWKBD_CANCELED);
}

HOST_OVERRIDE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", ov_show_keyboard);
