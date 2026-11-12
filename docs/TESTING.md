# How it was tested

**[Español](README.es.md)** is the Spanish overview; this page is English only.

Everything below was measured; the date and the source of each figure is listed.

## Whole-game sweep, not hand-played (Returns HD, 2026-11-11)

A tool warps directly into each level and lets the game's Super Guide clear it, recording every
frame.

- PC: 81 levels, 782,218 frames, 60 fps average in all of them, 99.966 % under 33.4 ms.
- Steam Deck OLED (SteamOS, Desktop Mode, CPU governor `performance` from level 20): all 81 levels, 805,215 frames,
  60.0 fps average (16.67 ms), 99.923 % of frames under 33.4 ms; APU power 8.0 W average, 8.9 W peak (sweep report
  of 2026-11-11). On the Deck's 90 Hz panel in Desktop Mode a 60 fps frame alternates between one and two refreshes,
  so 73.1 % of frames land under 16.9 ms even though the game advances exactly at 60 fps.

A **hitch** in the README is a frame that stays on screen longer than two refreshes (over 33.4 ms).

The remaining ≈0.08 % of Deck frames are the current known gap; the 99.99 % target is not claimed.

## Why the cap is 60 fps (measured 2026-11-11)

The game logic advances a fixed 1/60 s step per frame. Measured on level 14 at 90 Hz: the level
finished in 82.8 s versus 123.2 s at 60 Hz (1.49× faster) with the same frame count (7,421 vs 7,393).
60 fps is therefore the correct game speed, not a limitation of the port; higher rates need a
real-time step patch that is not done.

## Shaders are prepared before you play (Returns HD, 2026-11-11)

First start shows a blocking "Preparing shaders" screen (904 variants, 1.6–2.2 s on the PC above) and
it repeats only when the translator or the GPU driver changes. In a sweep with a deliberately empty
cache over levels 0, 13, 18, 27 and 48, the worst frames had **0 ms in shader translation and 0 ms in
pipeline compilation**: no level asked for a variant or pipeline outside the prepared list.

Tropical Freeze (measured 2026-11-12, fresh install, same three levels): shader preparation covers every program in the
game's files; 240 programs the game builds while it runs cannot be prepared ahead. Each level load still translated
28–34 shaders (worst stall 91–313 ms) and 2 more during play — a known issue (see KNOWN-ISSUES).

## Whole-game and level results: Tropical Freeze

Tropical Freeze: playable: boot, title, menus, cinematics and levels run on PC (level 1-1 measured at
16.67 ms average frame time, p99 21.24, 0 frames over 33.4 ms, 2026-11-12). It has been played through
a Steam shortcut on the Deck (2026-11-12).

Tropical Freeze has no Super Guide, so the whole-game sweep used for Returns HD cannot run on it and no whole-game
percentage is claimed. Measured on 2026-11-12 on a fresh install, three levels (1-2, the world-5 boss, a world-6 level):
16.67 ms mean frame time, p99 18.67–18.90 ms.

## Hardware tested

- PC: AMD RX 9070 XT, Mesa RADV 26.2.5, Wayland (2026-11-11/12 measurements).
- Steam Deck OLED (Galileo), SteamOS, measured in Desktop Mode (2026-11-11).
- The Deck LCD is detected and configured but has not been measured.

Steam Deck Game Mode with its default power settings: not measured yet.

## Human play sessions

- 2026-11-11, PC: ~10 minutes with a DualSense controller (responsive input, working rumble and audio).
- 2026-11-11, Deck: level 8-7 at ~59 fps with the APU at 5–6 W.
- 2026-11-12: both games played and declared ready by the owner.

## Media and I/O checks (2026-11-11)

- Audio stream: exactly 48,000 frames/s, zero underruns in 140 s.
- H.264 cinematics decoded with openh264, bit-exact against ffmpeg on 60 frames; the intro's audio
  track matches the recording with correlation 0.969.

## Deck reload

See [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for the Deck death/checkpoint reload measurement.
