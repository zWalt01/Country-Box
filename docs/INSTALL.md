# Installing Country-Box

**[Español](README.es.md)** is the Spanish overview; this page is English only.

Country-Box does not include any game: you need your own copy, **already extracted** into `exefs/` and `romfs/`.
It does not extract, decrypt or dump anything. See [../LEGAL.md](../LEGAL.md).

## With the launcher (recommended)

1. Download the AppImage from [Releases](https://github.com/zWalt01/Country-Box/releases) and make it executable
   (`chmod +x Country-Box-*.AppImage`).
2. Open it, choose **Add your copy**, and point it at the folder that holds `exefs/` and `romfs/`.
3. Press **PREPARE**. The first time it sets up its own Python modules (about 40 MB download), converts the game's
   code, compiles it and prepares the shaders. If a system tool is missing, it shows the exact command for your
   distribution.
4. Press **PLAY**.

With an AI agent: give it [AGENT-INSTALL.md](../AGENT-INSTALL.md) — it drives the launcher's command line
(`--status`, `--add`, `--prepare`, `--play`), which answers in JSON.

## Requirements

**Game copy** — Nintendo Switch version 1.0.0, no update applied:

| Game | Title ID | Version | Files checked | Tested with |
|---|---|---|---|---|
| Donkey Kong Country Returns HD | `01009D901BC56000` | 1.0.0 | 368 | US release |
| Donkey Kong Country: Tropical Freeze | `0100C1F0051B6000` | 1.0.0 | 88 | US release |

Other regions usually ship the same files on Switch (not verified). The launcher checks every file by size and SHA-256
and says plainly if one is missing, damaged, or from another version (usually an update applied on top).

**Computer** — Linux on x86-64:

- CPU with AVX2 (Intel Haswell / AMD Zen or newer) and a Vulkan GPU.
- Disk: about 4.2 GB for Returns HD and 1.6 GB for Tropical Freeze, plus your copy.
- `clang`, `lld`, `ninja`, `pkg-config`, `python3` (with `venv`), and the development packages of SDL3, Vulkan, Opus,
  libvpx and zlib.
- Internet once, on the first Prepare.

## Manual build (developers)

Without the launcher, from a clone of this repository. Building the shader translator needs the .NET 10 SDK.


```sh
git clone <this repository> && cd country-box/core
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt

# 1. Generate C from your extracted copy (output stays OUTSIDE the repository)
.venv/bin/python recomp/gen.py --exefs /path/to/exefs --out ../gen/dkcrhd --jobs 16

# 2. Build the shader translator (Ryujinx-derived, MIT; writes mp4_xlate)
.venv/bin/python tools/build_xlate.py

# 3. Compile the game + runtime into <out>/country-box
.venv/bin/python tools/build_linux.py --gen ../gen/dkcrhd \
    --exefs /path/to/exefs --romfs /path/to/romfs --out ../build/dkcrhd --jobs 14
```

## Running

Run `./country-box` from the build folder, with `exefs/` and `romfs/` next to the binary (or pass
`--exefs` / `--romfs`). The first start prepares the shaders (a blocking "Preparing shaders" screen),
and then the game boots. See [KNOWN-ISSUES.md](KNOWN-ISSUES.md) for when that repeats.

## Steam Deck

Compile on a PC and ship the result:

```sh
tools/deck_bundle.sh <build> <gen> <bundle>
```

This makes a portable folder (binary, translator, lists, checks). Put your `exefs/` and `romfs/`
next to it on the Deck. The script does not put anything from your game into the bundle.

## Settings

Settings are shared by the in-game overlay menu (F1) and the command line (`--settings-dump` /
`--settings-set`), stored in a single `mp4.ini`. Per-machine defaults: the Deck gets borderless
1280×800 with 720p render and FSR 1. Refresh rate is 60 Hz only; see [TESTING.md](TESTING.md) for why.

## Hardware

See [HARDWARE.md](HARDWARE.md).

## How long does the first build take? (measured 2026-11-12)

On an AMD Ryzen 7 5700X3D (8 cores / 16 threads) with all cores in use:

| Game | First build | After an update |
|---|---|---|
| Donkey Kong Country Returns HD | **9 min 21 s** (code conversion 1 min 46 s, compile ~7 min 25 s, shaders 2 s) | seconds: only what changed is rebuilt |
| Donkey Kong Country: Tropical Freeze | **2 min 31 s** | **10 s** with one runtime file changed (measured) |

A machine with fewer cores takes longer roughly in proportion. A Steam Deck has no compiler: build on a PC and use
"Import a build" (`--import-build`) on the Deck.
