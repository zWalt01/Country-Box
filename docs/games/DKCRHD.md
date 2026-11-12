# Technical dossier — Donkey Kong Country Returns HD (Switch)

Status: **under construction**. Each datum is either *measured* (with the command or source) or *assumed*.

## Identity

| Item | Value | Source |
|---|---|---|
| Title ID | `01009D901BC56000` (US) | measured: name of the dump's package |
| Supported version | v0, base game without updates | measured: dump status notes |
| Build ID of `main` | `8AC516E8A5D2564B8A2DFC11D3907B9F` | measured: bytes `0x40..0x60` of the NSO (2026-11-10) |
| exefs SHA-256 | `main` `5417520786a3a0bab8ff3326949b676b6c59a6bbee5afab94bdbeade60c61ebc`<br>`sdk` `497fd0ebea2b016ccd0edb6b4e2efa5fc262f2cff6438dd64c0cbaa26b22a90b`<br>`subsdk0` `8a2f970bcfc0d411526af185df22ab2c4e3a06e69d2dae649bcfc352fd6b48da`<br>`main.npdm` `1764f798d19e966e08f3d3b26b01d5239e1287baef32c60bba451cceb91ea542`<br>`rtld` `a43abf145ade81469158b536bd277377b60814a90ee5bebb00dc02b13996d70c` | measured: `sha256sum <your copy>/exefs/*`; seed of the manifest |
| romfs | 363 files, 15 GB; SHA-1 list kept outside the repository | measured |
| Engine | Unity **2022.3.42f1**, **IL2CPP** backend | measured: engine version strings in the data files and in the `main` module |
| Port developer | Forever Entertainment (2025) | assumed (from the dump status notes); not measured in the binary |
| Lineage | New remake in Unity — **does not** share code with the Wii version (2010, Retro engine) or the 3DS version (2013) | assumed from reasoning: different engine; to be confirmed |

## exefs (measured with `ls -la`)

| Module | Size on disk (compressed) |
|---|---|
| `main` | 39 980 263 B (~88 MB decompressed) |
| `sdk` | 5 976 954 B |
| `subsdk0` | 3 415 764 B |
| `rtld` | 8 180 B |
| `main.npdm` | 1 604 B |

## romfs (measured with `ls`, `du -sh`, `find`)

- Total **15 GB**, standard Unity layout: a `Data/` folder with the global data file, numbered level and shared asset files, `resources.assets`, `Managed/` and `StreamingAssets/`.
- **IL2CPP metadata is present** (`Data/Managed/Metadata/global-metadata.dat`, 9 727 320 B). This means the game's class and method names can be recovered. The analysis output is kept outside the repository and is not published.
- **Video**: `StreamingAssets/movies/*.mp4` (10 files, 56–162 MB). Playback goes through Unity's video player to the system H.264 decoder (assumed).

## Test with SwitchRecomp v190 (2026-11-10, measured in the run log)

- Loads modules, compiles 7 240 shaders with no failures, and renders 86 frames / 574 draws.
- Afterwards: `frames=86` stays fixed, `0.0 FPS`, GPU 0 % busy; the audio output stays `started` but with `peak=0.00000` (the audio queue runs **silently**).
- Favored hypothesis (not yet tested): the `.mp4` start video cannot be decoded because the runtime has no H.264 decoder (the optional OpenH264 is not linked). Next measurement to take: a capture during the stall.

## Modules after decompression (measured with a survey script)

| Module | Image | Symbols | Functions | Imports | Label |
|---|---|---|---|---|---|
| `main` (`GameAssembly.nss`) | 88 748 192 B | 52 (stripped) | 213 096 | 671 | `Unity-2022_3_42_f1` |
| `sdk` | 11 828 768 B | 22 044 | 32 077 | 8 | `NintendoSdk_nnSdk-18_3_2` |
| `subsdk0` | 5 667 699 B | 9 264 | 10 131 | 665 | `NintendoSDK_movie-18_3_2` |

- **SDK 18.3.2**. Graphics use NVN (`nvnBootstrapLoader`). Imports of `main` by namespace: `nn::os` 99, `nn::hid` 62, `nn::audio` 45, `nn::fs` 31, `nn::socket` 22, `nn::oe` 16.
- `main` ships without its symbol table; the IL2CPP metadata provides the names.
- **Video**: Unity uses its Switch video player, which maps to `nn::movie` in `subsdk0`. This is the same video layer used by Tropical Freeze and MP4; both existing recompilation projects already solved it (H.264 via OpenH264/libavcodec). This supports the stall hypothesis.

## Game time model (2026-11-10)

Sources: the Unity asset tools over the global data file, and a disassembly-based dumper run on the main module. Output is kept outside the repository.

| Item | Value | Status |
|---|---|---|
| Physics fixed timestep | 0.0333 s (**30 Hz**) | measured |
| Maximum allowed timestep | 0.1667 s | measured |
| Physics 3D and 2D simulation mode | **2 = Script** (the game decides when to simulate) | measured |
| Quality settings (4 levels) | `vSyncCount = 1` in all of them, so the target is 60 Hz | measured |
| Worker threads multiple | 8 | measured |
| Gameplay code | Gameplay code lives in a single game namespace, about 2 000 types; the logic is a C# port of the **3DS** version's code | measured for the naming; the 3DS origin is an inference |
| Logic signature | Update functions take an explicit time delta (`dt`), not a frame counter | measured |
| Fixed-update counter | the fixed-update step at 30 Hz counts frames | measured (signature) |
| Frame-rate modifier | a per-level modifier switches between 30 and 60 Hz by section | measured (signature) |
| Source of the `dt` | not resolved: no direct call to the engine's time query from the game assembly (IL2CPP probably inlines it) | **assumed**; to be verified at runtime by hooking the engine's time calls |

Implication for the Steam Deck: if `dt` is the real elapsed time, a dropped frame only makes the game look choppy and does not slow it down; the fixed cost is the 30 Hz step. If it were constant (1/60), each dropped frame would slow the game, and the 16.6 ms budget would become mandatory instead of desirable. This is the first runtime measurement to take.

## Still to measure

- Horizon services it requests (from the `main.npdm` metadata and an IPC log).
- IL2CPP symbol counts and subsystems (video, input, saves).
- Threads and core usage (`threads.txt`).
