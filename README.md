<div align="center">

# Country-Box

**Native Linux and Steam Deck ports of *Donkey Kong Country Returns HD* and *Donkey Kong Country: Tropical Freeze*,
built on your machine from your own copy.**

[Español](docs/README.es.md) · [Install guide](docs/INSTALL.md) · [Install with an AI agent](AGENT-INSTALL.md) ·
[Known issues](docs/KNOWN-ISSUES.md) · [About](ABOUT.md) · [Changelog](CHANGELOG.md)

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

![The Country-Box launcher](docs/images/launcher.webp)

*The launcher — its art is our own, drawn in code. Out of respect for the games' owners, this repository shows no
screenshots or video of the games themselves.*

</div>

I wanted to play these two games natively on my Steam Deck — simple to set up, and smooth once running — and I
couldn't find a recompilation of either. So I made one. The game's code is converted to C and compiled into a native
Linux program on your own computer.

> **Country-Box includes nothing from the games.** You bring your own copy; the launcher checks it, builds the game
> and runs it. Nothing here extracts or decrypts anything, and this project never says where to obtain a game.

## Requirements

**Your own copy of the game** — Nintendo Switch, no update applied, extracted into a folder with `exefs/` and `romfs/`:

| Game | Version | Region tested | Title ID |
|---|---|---|---|
| Donkey Kong Country Returns HD | 1.0.0 | US | `01009D901BC56000` |
| Donkey Kong Country: Tropical Freeze | 1.0.0 | US | `0100C1F0051B6000` |

Other regions usually ship the same files on Switch; the launcher checks your copy and tells you if it matches.

**A computer to build it on:**

- **Linux on x86-64** with a CPU that has AVX2 (Intel Haswell / AMD Zen or newer) and a Vulkan GPU.
- **Build tools** (`clang`, `lld`, `ninja` and a few libraries) — if one is missing, the launcher tells you the exact
  command for your distribution.
- **Steam Deck:** it runs the game, but it can't build it. Build on a Linux PC, then use **Import a build** on the Deck.

## Install

1. Download the AppImage from [Releases](https://github.com/zWalt01/Country-Box/releases) and make it executable.
2. Open it, choose **Add your copy**, and point it at your game folder.
3. Press **PREPARE** — about 9 minutes for *Returns HD* and 2½ for *Tropical Freeze* on an 8-core PC, once.
4. Press **PLAY**.

Updates install themselves and rebuild only what changed. Using an AI agent? Give it
[AGENT-INSTALL.md](AGENT-INSTALL.md). Full details: [docs/INSTALL.md](docs/INSTALL.md).

## Status — version 1.0.2

| | Returns HD | Tropical Freeze |
|---|---|---|
| Playable | ✅ whole game, PC and Steam Deck | ✅ PC and Steam Deck |
| Frame rate | 60 fps in all 81 levels; 99.9 % of frames without a hitch | 60 fps on the levels measured |
| Steam Deck | 60 fps with the APU at 8–9 W | plays well |
| Shaders | all prepared before you play | some still compile while a level loads — short pauses ([known issue](docs/KNOWN-ISSUES.md)) |
| 16:10 screen | 16:9 | ✅ native 16:10 |

How each number was measured: [docs/TESTING.md](docs/TESTING.md).

## About

I'm Walter, a data science student who wanted to find out how hard a project like this really is. I built it by
directing AI coding agents and checking every result against real runs — the story is in [ABOUT.md](ABOUT.md).
Questions and bugs: [Issues](https://github.com/zWalt01/Country-Box/issues).

Built on the recompilation core of [Metroid Prime 4 Recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp)
by Jeff Aigner and the projects behind it — full credits in [docs/CREDITS.md](docs/CREDITS.md).

## License and legal

MIT — see [LICENSE](LICENSE). *Donkey Kong Country Returns HD* and *Tropical Freeze* belong to Nintendo, Retro
Studios and Forever Entertainment; Country-Box is an independent, non-commercial project, not affiliated with them.
You need your own legally obtained copy. Full statement: [LEGAL.md](LEGAL.md).
