# Installing Country-Box

**[Español](README.es.md)** is the Spanish overview; this page is English only.

Country-Box does not include any game. Before you start, you need your own copy of the game,
**already extracted** into two folders: `exefs/` and `romfs/`. Country-Box verifies what you give it
against the supported version's hashes (Returns HD 1.0.0 US, title id `01009D901BC56000`). It does not
extract, decrypt or dump anything. See [../LEGAL.md](../LEGAL.md).

## Requirements

- Linux
- Python 3.12+
- `clang`, `lld`, `ninja`
- SDL3, Vulkan, opus and vpx development packages (plus zlib for Tropical Freeze)
- .NET 10 SDK, to build the shader translator

Measured build times on a 16-thread PC (2026-11-10/11):

- Returns HD: 79 s of code generation plus 384 s of compilation (544 files, 0 errors).
- Tropical Freeze: 40 s plus 3 min 32 s.

## Build steps

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
