# Changelog

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
