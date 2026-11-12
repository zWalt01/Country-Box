// From fe3h-pc-recomp runtime/input.h @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only.
// From totk-pc-recomp runtime/input.h @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: SDL3 window events replace the Win32 window procedure hooks (Linux).
// Host input sampling (runtime/input_sdl.c): SDL3 gamepad and the keyboard as one Switch npad.
#pragma once
#include <stdint.h>

// Bits of libnx HidNpadButton (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15) and the stick directions (StickLLeft..Down 16-19, StickRLeft..Down 20-23, derived from the sticks).
// Sticks are -32767..32767, up is +y. MP4_INPUT=none reports no input. Callable from any thread.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
// The same for host UI (the text entry): never blocked. input_block(1) while a text entry is open: input_poll then
// reports nothing; input_block(0) when it closes: nothing until all buttons and stick directions are released.
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void input_block(int on);
// The in-game menu (ui/menu.h): input_menu(1) when it opens (input_poll reports nothing, the cursor is released), (0) when
// it closes (nothing until every button and stick direction is released). Independent of input_block.
void input_menu(int open);
// The gamepad alone, for the menu: Switch button bits and the left stick (up +y). 0 without a connected gamepad.
// Minus+Plus combo and the neutral pad while the menu is open (input_map.c, window-system free): *held is the caller's memory
// of the combo, sticks are lx, ly, rx, ry. Returns 1 in the poll where the combo goes down (toggle the menu once).
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4]);
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
// The gamepad layout in effect (PAD_LAYOUT_*, input_map.h; MP4_PAD_LAYOUT, live); -1 without a gamepad. Any thread.
int input_pad_layout(void);
// The label on a face button of the open gamepad (input_map.h PAD_SOUTH..PAD_NORTH, as SDL reports it); PAD_LABEL_NONE for another
// button, an unknown label or no gamepad. Any thread.
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y, PAD_LABEL_CROSS, PAD_LABEL_CIRCLE,
       PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
int input_pad_label(int pad);
// The menu's remapping rows (Controls tab). input_map_snapshot copies the tables in effect (after any changed MP4_BIND_* line).
// input_capture polls the keys, the mouse buttons and the gamepad for a press: reset 1 takes what is held now as the
// start (a held key counts once released and pressed again) and returns NONE; then it returns KEY or PAD with *id the
// key or pad id (input_map.h) pressed since the last call, or CANCEL for Esc. Keys count only while the game window is in front.
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD, INPUT_CAPTURE_CANCEL };
struct InputMap;   // input_map.h (not included here: swkbd_sdl.c has button names of its own)
void input_map_snapshot(struct InputMap *m);
int input_capture(int reset, int *id);
// The character on a punctuation key (KEY_MINUS..KEY_GRAVE) in the keyboard layout in use, 0 for another key.
uint32_t input_key_char(int key);

// ---- the SDL window thread (gfx_window.c). It owns the window and its event loop and never runs guest code; every
// event goes to input_window_event first (key and mouse button state, raw mouse motion, focus, cursor release), then
// to menu_window_event (ui/menu_sdl.cpp: F1 / Esc open or close the menu; while it or a text entry is open, keys,
// mouse, text and focus go to it), then to the window's own handling. Each returns 1 when the event is fully handled.
// The cursor is hidden and confined (SDL relative mouse mode) only by the window thread: input_cursor_apply.
// input_cursor_suspend(1) releases the cursor around a fullscreen change, (0) captures it again.
union SDL_Event;
int input_window_event(const union SDL_Event *e);
int menu_window_event(const union SDL_Event *e);
void input_cursor_apply(void);            // window thread: relative mouse mode as the capture state wants it
int input_cursor_released(void);          // Esc or a focus change with a mouse button held released the cursor (probe)
// What the window thread offers the input side; any thread.
int gfx_window_on_thread(void);           // 1 on the window thread
void gfx_window_cursor_sync(void);        // the window thread runs input_cursor_apply soon (at once on that thread)
void gfx_window_text_input(int on);       // SDL text input on the window (the Steam Deck shows its keyboard); waits
// Runs e through the window thread's handling as if SDL had queued it and returns once it was handled (probes; not on
// the window thread itself).
void gfx_window_dispatch(const union SDL_Event *e);
void input_cursor_suspend(int on);

// Rumble: SDL_RumbleGamepad on the open gamepad, low (heavy, left) and high (light, right) motor 0..65535 for ms
// milliseconds. 1 if a gamepad with rumble took it. Callable from any thread.
int input_rumble(uint16_t low, uint16_t high, uint32_t ms);
// Motor value for a normalised amplitude: pow(a, curve) * strength, saturated to 0..65535. Pure.
uint16_t rumble_motor(float a, float curve, float strength);
// MP4_RUMBLE_TRIGGERS=1: input_rumble also drives the impulse triggers (Xbox) with the high motor's value.
int input_rumble_triggers_enabled(void);

// Motion (MP4_MOTION=0 turns it off, live). input_motion_start starts the sensor thread once: every 4 ms it reads the
// gamepad's gyro and accelerometer (when it has them) and feeds motion_global(). Callable from any thread.
void input_motion_start(void);
// MP4_MOTION is not "0" (a live setting: read again when cfg_version changes). Off: the sensor reads at rest.
int input_motion_enabled(void);
// SDL sensor values (accelerometer m/s^2, gyro rad/s, SDL gamepad axes) to the Switch controller frame motion_feed
// takes (acceleration in G as gravity reads, rotation rad/s). The mapping is documented in input_sdl.c.
void input_motion_axes(const float sdl_acc[3], const float sdl_gyro[3], double acc_g[3], double gyro[3]);

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);   // the gamepad's, fed by input_sdl.c
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms
void motion_restart(MotionFusion *m);   // angle and direction start over at the next sample
