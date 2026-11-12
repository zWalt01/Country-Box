# Supported hardware

**[Español](README.es.md)** is the Spanish overview; this page is English only.

## Steam Deck

- OLED: measured. LCD: detected and configured, not measured.
- Default: borderless 1280×800 with 720p internal render. In handheld mode the game itself draws
  1280×720 with identical framing and HUD (measured, level 13, 2026-11-11).
- AMD FSR 1 upscaling with sharpness control.
- The port is CPU-bound on the Deck, not GPU-bound: CPU 16–21 ms per frame, GPU 3–6 ms at ≈6 W
  (2026-11-11).
- Measured in Desktop Mode. The Deck OLED in Desktop Mode runs its panel at 90 Hz without VRR; see
  [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

Steam Deck Game Mode with its default power settings: not measured yet.

## Linux PC

- Requires Vulkan. The renderer is Vulkan-only.
- Measured on AMD RX 9070 XT with Mesa RADV 26.2.5 on Wayland (2026-11-11/12 measurements).

## CPU

- x86-64-v3 (the Steam Deck's Zen 2 and anything recent).

## Audio and controllers

- Audio via SDL3.
- Controllers via SDL3 (gamepads, rumble, gyro).

## Widescreen

Native 16:10 (`video.aspect=native`): Tropical Freeze renders the world at 16:10 keeping the horizontal view, with the
HUD centred at 16:9; its video cinematics are stretched 11 % (known issue). Returns HD: not available yet (16:9).

## Refresh rate

Refresh rate is 60 Hz only. 90 Hz and "variable" are hidden until the real-time step patch exists,
because at 90 Hz the game runs 1.49× too fast (measured; see [TESTING.md](TESTING.md)).
