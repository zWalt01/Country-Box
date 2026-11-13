# Known issues

**[Español](README.es.md)** is the Spanish overview; this page is English only.

- **2–4 hitches per level, 50–146 ms** (worst measured 145.7 ms, level 9-9; 2026-11-11): shader
  translation and pipeline time in those frames is 0.0 ms, so this is the game's own CPU work (under
  investigation).
- **Death/checkpoint reload hitch on the Deck**: ~70 ms (~4,300 page faults) versus 25 ms on the PC
  (measured 2026-11-11).
- **Deck OLED in Desktop Mode** runs the panel at 90 Hz without VRR: 60 fps alternates between
  refreshes (11.1/22.2 ms), visible judder even though game pacing is exact. Use Game Mode at 60 Hz.
- **First start blocks on shader preparation** (by design), and repeats it after a Mesa/SteamOS update
  changes the driver identity.
- **Tropical Freeze cinematics**: frame pacing under review (one measured cutscene dropped 315 of
  4,471 frames to keep time, 2026-11-11).
- **Tropical Freeze shader stutter on level load**: 28–34 shaders are still translated while each level loads (worst
  stall 91–313 ms) and ~2 during play, on a fresh install (measured 2026-11-12). They come from a few bytes the game
  rewrites in each shader's control data at every boot; a fix is in progress.
- **Tropical Freeze video cinematics at 16:10** are stretched 11 % taller (the in-engine scenes and the HUD are fine).
  Use the 16:9 picture (`video.aspect=fit`) if it bothers you.
- **CPU requirement**: the build targets x86-64-v3 (AVX2, FMA, BMI2): Intel Haswell (2013) or newer, AMD Zen or newer,
  Steam Deck. Older CPUs, and ARM, are not supported.
- **The first build is long**: about 9 minutes for Returns HD on an 8-core PC (see INSTALL). A Steam Deck has no
  compiler: build on a PC and import the build.
- **Save states are experimental** (1.0.4): they have not been deeply tested yet, so they may not work everywhere.
  F5 / F6 or Select+Start → menu. They freeze the whole game and write it to `profile/savestates/` (about 1.7–2 GB
  each, the last 3 kept): under a second on a fast SSD (measured on PC, 2026-11-13), longer on a microSD card. They
  only load while the game stays open, and a load is refused (nothing changes) when the game started or ended threads
  or changed its memory pools since the save, which can happen after a level change. Sound may skip for a moment
  after a load. Not tested yet on the Steam Deck. Feedback is needed to polish them: report what failed, with the
  game and level, in [#3](https://github.com/zWalt01/Country-Box/issues/3) or a new issue.
- **PC OPTIONS inside the game's own Options menu** exist but are off in 1.0 (`CB_NATIVE_OPTIONS=1` turns them on).
