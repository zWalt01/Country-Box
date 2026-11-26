# Changelog

## 1.0.9 — 2026-11-25

Heads up: you may see a bit more movement in Country-Box from now on. My final exams are over (and they went
well!), so I have more time for it again.

- **Tropical Freeze's particle effects are back (experimental)** — bubbles underwater, balloon pops, the clouds and
  stars when an enemy is defeated, banana coin shine, smashed rocks. The game draws them with *geometry shaders*,
  a shader stage Country-Box ignored, so every particle reached the GPU as a bare dot. Contributed by
  [@llethas](https://github.com/llethas) ([#27](https://github.com/zWalt01/Country-Box/pull/27)), combined with our
  own branch for the same fix ([#17](https://github.com/zWalt01/Country-Box/issues/17)). Tropical Freeze prepares its
  shaders once more after updating. A short hitch can still happen the first time an effect appears.
- **Local two-player co-op (experimental, off by default)** — a second controller joins as player 2 in both games.
  Contributed by [@oSyphon](https://github.com/oSyphon) ([#25](https://github.com/zWalt01/Country-Box/pull/25)). Turn
  it on with `MP4_COOP=1` (Steam launch options: `MP4_COOP=1 %command%`) or `coop = 1` in the game's `mp4.ini`.
  Player 2 has no rumble or gyro yet.

## 1.0.8 — 2026-11-22

- **Preparing a game works again when the AppImage is in a folder with spaces** (for example
  `~/Downloads/Tropical Freeze/`). Since 1.0.7 the build tools live next to the AppImage, and the bundled build kit
  (used on the Steam Deck and on PCs without a compiler) split that folder's name in two, so the compiler stopped with
  "no such file or directory". Found and fixed by [@llethas](https://github.com/llethas)
  ([#20](https://github.com/zWalt01/Country-Box/issues/20), [#21](https://github.com/zWalt01/Country-Box/pull/21)) —
  thank you!

## 1.0.7 — 2026-11-21

- **Windows support is on its way, thanks to [@testsenvergonha-sys](https://github.com/testsenvergonha-sys)**
  ([#19](https://github.com/zWalt01/Country-Box/pull/19), closes [#5](https://github.com/zWalt01/Country-Box/issues/5)).
  They ported Country-Box's core and launcher to Windows x86_64, a huge piece of work that I honestly never expected
  anyone to do for this project. Thank you so much! The code is in this version; a Windows download isn't ready yet
  (I don't have a Windows machine to build and test it), but it's coming very soon.
- **Everything in one folder.** Country-Box now keeps everything it writes (your games' builds, saves, settings,
  logs, its own Python and temporary files) in one folder, `Country-Box-data`, next to the AppImage (or next to the
  extracted tarball). Delete that folder and Country-Box is gone completely; nothing is left in your home folder.
- **You choose where, and your saves stay safe.** The first time you open 1.0.7 with your current library, it asks:
  keep it where it is (nothing changes, and it won't ask again), move it next to Country-Box, or move it to a folder
  you pick. Before anything moves, **a copy of your saves goes to your Desktop** (`Country-Box saves backup <date>`,
  with a note on how to put them back), and the saves move with your games too, so they end up in two places.
  *Move library* (About tab) works the same way. Starting a game straight from Steam never moves anything.
- After moving, each game's next *Prepare update* rebuilds it from scratch (about 10 minutes for Returns HD on my
  PC, under 3 for Tropical Freeze).
- **A tarball download** next to the AppImage (`Country-Box-linux-x86_64.tar.gz`), for those who prefer not to use
  AppImages. It updates itself the same way.
- **Command-line options for everything** (`--help` lists them): `--skip [GAME]` starts the game you played last
  without opening the launcher; `--fullscreen`, `--windowed` and `--resolution WxH` for that run; `--data-dir DIR`;
  `--no-update`.

## 1.0.6 — 2026-11-17

Heads up: this one is **lightly tested**. I'm buried in university exams right now, so instead of sitting on these
fixes until I can test every corner, I'm putting them out so anyone who needs them can use them today. If something
breaks, please open an issue (or, if you figure it out yourself, even better: tell me how!). Thanks for your
patience, and wish me luck on my exams.

- **Rumble in Tropical Freeze.** It never reached any controller: the game asks for a different kind of vibration
  handle than Returns HD, and Country-Box dropped all of them. It vibrates now (tried with one real controller). The
  log also says what happened with the first rumble (`input: first rumble …: the gamepad took it`, or why not)
  ([#8](https://github.com/zWalt01/Country-Box/issues/8)).
- **Fewer shader hitches in Tropical Freeze.** The game rewrites a few bytes of each shader at every launch, and those
  bytes made the same shader look new every time. They're ignored now and the shipped lists were rebuilt: on a clean
  install, the shaders translated while playing a level dropped from 74–90 to 16 (the 16 left are built by the game at
  run time and can't be prepared ahead). Tropical Freeze prepares its shaders once more after updating, and it's
  faster too (about 10.7 s → 6 s on my PC) ([#13](https://github.com/zWalt01/Country-Box/issues/13)).
- **Installing from RecompLauncher.** It didn't work properly: after picking your copy, setup stopped with "Its setup
  program is missing", so installing and launching from RecompLauncher wasn't reliable. Country-Box's RecompLauncher
  file pointed at the wrong program. That file is fixed now, and RecompLauncher reads it straight from this
  repository, so the fix applies without waiting for a new version. This should fix it; I'm on it and testing it on
  a Steam Deck right now.
- **Steam Deck: prepare the game on the Deck itself.** SteamOS has no compiler, so until now a Deck could only play a
  build made on a PC. Country-Box now carries the tools it needs to prepare the game. They are used only while a game
  is being prepared, and nothing is installed on your system. Because of that, **the download grows from about 10 MB
  to about 200 MB**. This is the least tested part of the release: I haven't had the time to run a full Prepare on a
  real Deck yet, so if you try it, please tell me how it went in
  [#15](https://github.com/zWalt01/Country-Box/issues/15).
- **An update never takes PLAY away.** After updating Country-Box, your games keep playing as before; *Prepare
  update* is optional and brings the newest fixes when you like. Until now the card asked you to prepare first, which
  on a Steam Deck meant you couldn't play.
- **B exits** Country-Box from the Games tab (handy on the Steam Deck).
- About: the text no longer runs past the bottom of the panel on the Steam Deck's screen.
- Steam Deck: with FSR on, the picture looked blocky. The Deck's screen is only slightly taller than the game's
  picture, and FSR is made for bigger enlargements; for small ones the regular scaling is used now, which looks
  smoother.
- Start a game directly, skipping the launcher: `--returns` or `--freeze` (also `--play returns` / `--play freeze`),
  handy as Steam launch options or in a desktop shortcut. If the game can't start yet, a window tells you why
  ([#6](https://github.com/zWalt01/Country-Box/issues/6)).
- RomFS mods (`MP4_ROMFS_MODS`) work again: an overlay that replaces a game file was always refused, because the
  original was hashed from a packed image that these builds don't use. Thanks to cat99sky
  ([#9](https://github.com/zWalt01/Country-Box/pull/9)).
- For developers: the `MP4_TRACE=perf` slow-frame line also shows `drain` and `other` (the frame time not spent
  waiting), so a slow frame's time adds up ([#12](https://github.com/zWalt01/Country-Box/issues/12)).

## 1.0.5 — 2026-11-14

- *Donkey Kong Country Returns HD* now accepts copies of the **physical cartridge**. Until now the launcher only
  recognised the eShop version and called a cartridge copy incompatible, even though it is the same game
  ([#7](https://github.com/zWalt01/Country-Box/issues/7)).
- After updating, each game asks for a quick Prepare (a few seconds on PC; your saves and settings stay).
- A reminder: **save states are still experimental** (F5 saves, F6 loads; Select+Start opens the menu). I haven't
  received any feedback on them yet, so if you try them, please tell me how it went in
  [#3](https://github.com/zWalt01/Country-Box/issues/3) or a new issue — even "it worked fine" helps.

Thank you for playing and for every report so far. Any feedback is appreciated: it's what helps me make Country-Box
better.

## 1.0.4 — 2026-11-13

- **Save states (experimental).** F5 saves, F6 loads; with a controller, Select+Start opens the menu with *Save
  state*, *Load state* and *Load older state*. The last 3 states are kept, so a save made mid-fall can be undone, and
  they only work while the game stays open (closing it deletes them). They have not been deeply tested yet, so I
  can't promise they work in every level or on every machine: a load can be refused after some level changes, the
  sound may skip for a moment, and each state is about 2 GB on disk (under a second on a fast SSD, longer on a
  microSD card). **Feedback is needed to polish them**: if a save or load fails or does something odd, please tell me
  in [#3](https://github.com/zWalt01/Country-Box/issues/3) or a new issue, with the game, the level and what happened.
- Folder picker: the list scrolls (mouse wheel, drag, d-pad), you can paste or type a folder path and press Enter, and
  on the desktop there is a button for your system's own file picker
  ([#1](https://github.com/zWalt01/Country-Box/issues/1)).
- Folders with spaces or other characters in their path (portable mode included) build again
  ([#2](https://github.com/zWalt01/Country-Box/issues/2)).
- About → *Move library…*: choose where the builds and saves live; everything moves there, saves included
  ([#2](https://github.com/zWalt01/Country-Box/issues/2)).
- The AppImage runs on older distributions now (built on Ubuntu 22.04: glibc 2.35 or newer; started on Ubuntu 22.04,
  Debian 12 and Fedora). 1.0.3 did not start on Ubuntu 22.04.
- Prepare no longer needs the `lz4` Python package (it had no ready-made package for Python 3.15, e.g. on current
  Fedora).
- The Prepare screen keeps its text clear of the buttons at any window size.
- After updating, each game asks to be prepared once more (only the game's runtime is rebuilt; saves and settings
  stay).

## 1.0.3 — 2026-11-12

- RecompLauncher's Easy Setup: you pick the folder of either game and the launcher tells which one it is
  (`country-box-launcher --setup auto=FOLDER`). To add the other game, pick its folder too; both stay set up.

## 1.0.2 — 2026-11-12

- RecompLauncher support: the AppImage carries `recomplauncher.json`, so [RecompLauncher](https://recomplauncher.com)
  can set Country-Box up from your copy, build it, show its main settings and start each game.
- New for launchers and scripts: `country-box-launcher --setup dkcrhd=FOLDER dkctf=FOLDER` checks each copy and
  prepares it without a window, printing `progress` lines. Nothing changes if you don't use it.
- The release file is now named `Country-Box-1.0.2-linux-x86_64.AppImage`. Updating from 1.0.1 works as before.

## 1.0.1 — 2026-11-12

- The launcher now updates itself when it runs as an AppImage: it downloads the new AppImage from the release,
  checks its SHA-256, replaces the file you run (the previous one is kept next to it as `.old`) and restarts.
  1.0.0 could not do this, so if you have 1.0.0, download 1.0.1 by hand once.

## 1.0.0 — 2026-11-12

First release: *Donkey Kong Country Returns HD* and *Donkey Kong Country: Tropical Freeze* on Linux and the Steam
Deck, built on your machine from your own copy, with the launcher (add your copy, Prepare, Play, updates, Import a
build). Known issues: [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md).
