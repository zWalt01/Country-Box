# Changelog

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
