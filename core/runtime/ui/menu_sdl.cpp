// From fe3h-pc-recomp runtime/ui/menu_win.cpp @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied unchanged.
// From totk-pc-recomp runtime/ui/menu_win.cpp @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied unchanged. Country-Box: renamed from menu_win.cpp; SDL3 window events instead of Win32 messages (Linux).
// SDL3 adapter of the in-game menu: window events to menu events (menu.h). The only window-system code of the menu.
// Window thread.
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_window_event runs first; closing the menu gives the capture back). While the menu is open, its keys,
// the mouse and focus changes go to it; the game polls the keyboard on its own (input_sdl.c) and gets nothing while the
// menu is open (input_menu). While a text entry is open (swkbd_sdl.c) its keys, typed text and the mouse go to it
// instead, and F1 / Esc do not toggle the menu (Esc cancels the entry).
#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "input.h"
#include "ui/menu.h"
}

// Navigation keys by what they do (keycodes: the arrows and the editing keys sit anywhere on a layout); Q and E switch
// tabs as labelled, as the Win32 virtual keys did.
static ImGuiKey key_of(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_Q: return ImGuiKey_Q;   // previous tab
    case SDLK_E: return ImGuiKey_E;   // next tab
    case SDLK_A: return ImGuiKey_A;   // the text entry's Ctrl+A, C, V, X, Z
    case SDLK_C: return ImGuiKey_C;
    case SDLK_V: return ImGuiKey_V;
    case SDLK_X: return ImGuiKey_X;
    case SDLK_Z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

// Window coordinates (points) to the swapchain's pixels, which the menu lays out in.
static void to_pixels(SDL_WindowID id, float *x, float *y) {
    SDL_Window *w = SDL_GetWindowFromID(id);
    float d = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    if (d > 0) *x *= d, *y *= d;
}

extern "C" int menu_window_event(const SDL_Event *e) {
    int entry = menu_text_entry_active();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && !entry && !e->key.repeat &&
            (e->key.key == SDLK_F1 || (e->key.key == SDLK_ESCAPE && !menu_is_open()))) {
            // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
            menu_toggle();
            return 1;
        }
        if (e->key.key == SDLK_F10) {
            if (down && !e->key.repeat) menu_perf_cycle();
            return 1;
        }
        if (!menu_is_open() && !entry) return 0;
        if (e->key.repeat) return 0;   // ImGui repeats a held key itself
        if (ImGuiKey k = key_of(e->key.key)) menu_event_key(k, down);
        // the modifier state ImGui reads for Shift+Tab and the text entry's Ctrl shortcuts
        if (e->key.key == SDLK_LCTRL || e->key.key == SDLK_RCTRL) menu_event_key(ImGuiMod_Ctrl, (e->key.mod & SDL_KMOD_CTRL) != 0);
        if (e->key.key == SDLK_LSHIFT || e->key.key == SDLK_RSHIFT) menu_event_key(ImGuiMod_Shift, (e->key.mod & SDL_KMOD_SHIFT) != 0);
        return 0;   // F11 and the window's keys keep working
    }
    case SDL_EVENT_TEXT_INPUT:
        if (entry) menu_event_text(e->text.text);
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        if (menu_is_open() || entry) {
            float x = e->motion.x, y = e->motion.y;
            to_pixels(e->motion.windowID, &x, &y);
            menu_event_mouse_pos(x, y);
        }
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!menu_is_open() && !entry) return 0;
        uint8_t sb = e->button.button;
        if (sb != SDL_BUTTON_LEFT && sb != SDL_BUTTON_RIGHT && sb != SDL_BUTTON_MIDDLE) return 0;
        int b = sb == SDL_BUTTON_LEFT ? 0 : sb == SDL_BUTTON_RIGHT ? 1 : 2;
        float x = e->button.x, y = e->button.y;
        to_pixels(e->button.windowID, &x, &y);
        menu_event_mouse_pos(x, y);
        menu_event_mouse_button(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return 0;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (menu_is_open() || entry) {
            float dx = e->wheel.x, dy = e->wheel.y;
            if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) dx = -dx, dy = -dy;
            menu_event_wheel(dx, dy);
        }
        return 0;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        menu_event_focus(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        return 0;
    }
    return 0;
}
