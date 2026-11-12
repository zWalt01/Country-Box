// Country-Box launcher: all of the launcher's pictures, drawn in code with Dear ImGui's draw lists. Our own art only
// (nothing owned by others): a wooden barrel with "CB" carved in, bananas, palm leaves, jungle colours. Nothing from
// the games, not even read from the player's copy.
#pragma once
#include "imgui.h"

namespace art {

// Jungle palette
constexpr ImU32 kBanana = IM_COL32(246, 201, 69, 255);
constexpr ImU32 kBananaDark = IM_COL32(196, 140, 30, 255);
constexpr ImU32 kWood = IM_COL32(139, 90, 43, 255);
constexpr ImU32 kWoodDark = IM_COL32(84, 50, 20, 255);
constexpr ImU32 kWoodLight = IM_COL32(184, 128, 70, 255);
constexpr ImU32 kLeaf = IM_COL32(46, 125, 60, 255);
constexpr ImU32 kLeafDark = IM_COL32(20, 66, 36, 255);
constexpr ImU32 kCream = IM_COL32(250, 242, 220, 255);
constexpr ImU32 kInk = IM_COL32(32, 22, 12, 255);
constexpr ImU32 kGood = IM_COL32(92, 196, 92, 255);
constexpr ImU32 kWarn = IM_COL32(244, 168, 50, 255);
constexpr ImU32 kBad = IM_COL32(232, 86, 64, 255);
constexpr ImU32 kInfo = IM_COL32(96, 176, 236, 255);
constexpr ImU32 kMuted = IM_COL32(170, 178, 160, 255);

ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 alpha(ImU32 c, float a);

// The whole window's backdrop: jungle gradient, light shafts, palm fronds in the corners (t: seconds, for a slow sway).
void background(ImDrawList *dl, ImVec2 min, ImVec2 max, float scale, float t);
// The Country-Box barrel: centre and height; carved text drawn with font (the biggest loaded).
void barrel(ImDrawList *dl, ImVec2 c, float h, ImFont *font, const char *carved = "CB");
void banana(ImDrawList *dl, ImVec2 c, float size, float angle);
// One frond: from base, pointing at angle (radians, 0 = right, y down), with droop (+ bends toward screen bottom).
void frond(ImDrawList *dl, ImVec2 base, float angle, float length, float width, float droop, ImU32 col);
void palm(ImDrawList *dl, ImVec2 foot, float height, float lean, ImU32 trunk, ImU32 leaves);
// A game card's picture (generic scenery for the game: id "dkcrhd" a warm island sunset, "dkctf" a frozen island).
void card_scene(ImDrawList *dl, ImVec2 min, ImVec2 max, const char *game, float scale, float t);
// Icons
void icon_check(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
void icon_alert(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
void icon_folder(ImDrawList *dl, ImVec2 min, ImVec2 max, ImU32 col);
void icon_gear(ImDrawList *dl, ImVec2 c, float r, ImU32 col, ImU32 hole);
void triangle(ImDrawList *dl, ImVec2 c, float r, int dir, ImU32 col);   // dir: -1 left, +1 right, 2 down
// A controller button for the hint bar: a disc with a letter (A, B, X, Y) or a pill with a word (LB, RB).
void pad_glyph(ImDrawList *dl, ImVec2 c, float r, const char *label, ImU32 col, ImFont *font, float font_size);
void spinner(ImDrawList *dl, ImVec2 c, float r, float t, ImU32 col, float thick);

}  // namespace art
