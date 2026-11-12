// Country-Box: the blocking "Preparing shaders" screen (ui/prep_screen.cpp), drawn by the menu overlay
// (ui/menu.cpp menu_overlay_frame) over a cleared swapchain image (gfx_window.c vk_present_screen) while
// shader_prep_boot runs the preparation. Plain text in the runtime's own font and style; nothing from the game.
// It takes no input: there is nothing to choose, the game starts by itself when the preparation ends.
#pragma once
#include <stdint.h>

#include "shader_prep.h"

#ifdef __cplusplus
extern "C" {
#endif

void prep_screen_begin(const char *game);                  // the screen shows from the next present on
void prep_screen_set(const ShaderPrepProgress *p);         // the latest progress; any thread
void prep_screen_end(void);
int prep_screen_active(void);
// Lays the screen out inside the overlay's ImGui frame (present path only). scale: the menu's (1.0 at 1080 lines).
void prep_screen_layout(uint32_t w, uint32_t h, float scale);
// Test hook CB_PREP_SHOT=<png path>[:<ms>]: the frame laid out that long after the screen began (default 3000 ms) is saved
// there once (the menu's shot path). NULL on every other frame.
const char *prep_screen_shot(void);

#ifdef __cplusplus
}
#endif
