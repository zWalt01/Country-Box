# Technical dossier — Donkey Kong Country: Tropical Freeze (Switch)

Status: **under construction**. Each datum is either *measured* (with the command or source) or *assumed*.

Measurement was done with one-off scripts that decompress the NSO in memory and print only counts and labels; nothing is written to the repository.

## Identity

| Item | Value | Source |
|---|---|---|
| Title ID | `0100C1F0051B6000` | measured: dump status notes |
| Version | v0 base game | measured: dump status notes |
| Engine | **Retro Studios' own engine**: a `GameFramework` module and a `full_rs12` build pipeline | measured: build paths embedded in `main` |
| Lineage | Same `GameFramework` as *Metroid Prime 4: Beyond* (`RS15`) and the *Prime Remastered* line; `RFRM` container in both | measured here + `docs/plan.md` of jaigner-hub/metroid-prime-4-recomp @ `6c9c590` |
| SDK | **NintendoSDK 4.4.0** (2018), much older than MP4 (19.3) and Returns HD (18.3.2) | measured: SDK release string in `sdk` |
| Online | NEX 4.2.1 + libcurl, `nn::socket`/`nn::nifm` imported (online markers); networking must be stubbed | measured: NEX version strings in `main` |

## exefs (measured)

| Module | Decompressed image | Symbols | Functions | Imports |
|---|---|---|---|---|
| `main` | 27 482 456 B | **68 496** (C++ engine symbols) | 59 014 | 652 |
| `sdk` | 10 102 616 B | 19 595 | 21 765 | 13 |
| `subsdk0` (`NintendoSDK_movie`) | 6 052 776 B | 10 243 | 9 806 | 637 |

- **`main` ships its symbol table**: about 68 000 names from the Retro engine. This gives an almost complete map of the code, which is rare. The names are not reproduced in this document.
- Imports of `main` by namespace: `nn::audio` 132, `nn::os` 68, `nn::hid` 38, `nn::socket` 29, `nn::fs` 26, `nn::account` 19, `nn::oe` 14, `nn::vi` 8.
- Graphics: **NVN** through `nvnBootstrapLoader`; `sdk` includes the `glslc_*` hooks.
- Video: `subsdk0` is `nn::movie` 4.4.0. The romfs contains `movies/*.mp4` (H.264, assumed from the extension).

## romfs (measured with `ls`, `du -sh`)

- Total **6.7 GB**. Top level: `MaterialArchive.arc`, `MiscData.pak`, `PreLoadPak.pak`, `UI/`, `Worlds/` (`w01_mangrove` … `w06_frozen`, `w0x_trophy`, `w0y_Secret`, `Map`), `movies/`.
- The same top-level layout as MP4 (`MaterialArchive.arc`, `MiscData.pak`, one `.pak` per world).

## Port with `core/` (branch `tropical-freeze`, 2026-11-11)

The dump has `exefs` and `romfs` folders (romfs: 83 files, 7 097 569 430 B). Derived outputs live outside the repository in the local build area `<build dir>`.

### Lift and build (*measured*)

| Step | Result |
|---|---|
| Lifter `recomp/gen.py` (low priority, 4 jobs) | 40 s; main 59 655 functions / 218 MB of C, subsdk0 13 329 / 57 MB, sdk 28 225 / 78 MB; 0 unsupported instructions |
| `tools/build_linux.py` | the game build is 99 MB, in 3 min 32 s (226 objects); rebuilding only the runtime takes seconds |
| Memory layout | main 0x7000000000 (0x64a2000), subsdk0 0x7006600000, sdk 0x7006e00000 |

### Milestones (*measured* on a development PC, RX 9070 XT, RADV)

1. **`--boot` passes**: nnMain up to the first call to `nvnBootstrapLoader` from main (228 SVCs, 9 threads).
2. **First frame**: title screen (L+R), 58 fps mean over 30 s.
3. **English title and full 3D scene** (beach, palm trees, DK's cabin).
4. **Menus with scripted input** (`MP4_INPUT_SCRIPT`): title → controller select → "Play in single player?" → character select → loading → first cutscene.
5. **Full opening cutscene** (~150 s, 720p H.264 with B-frames, AAC audio through the SDK) and **loading of 1-1 Mangrove Cove** (the pause screen shows "1-1 MANGROVE COVE"). Current blocker: the 1-1 intro stalls at its second camera shot (see the tracked items).

### What was needed (each item with its measured cause)

| Failure | Cause | Fix |
|---|---|---|
| `boot: no first-NVN-call checkpoint` | boot tables differ per version; TF and Returns are both 1.0.0 | `boot.c`: table per program ID and version (nvn_ret, init_array 217/45/105) |
| `nn::init::Start` not exported | SDK 4.4.0 exports the two-callback form | `boot.c` uses the two-callback form when the three-callback one is absent |
| `sm: service "set"` | the game's context asks for language codes (SDK 4.4 has no per-symbol override) | `set` service in `ipc_services.c` |
| Hangs after `nvnDeviceGetInteger` | the graphics thread exits: "NVN API version does not match driver version"; TF requires 53.105 (Returns/TotK 55.15) | NVN version per game in the table in `boot.c` |
| `audren: update output size` | REV4 has no render-info block in the output (16 B fewer) | `audren.c`: render info only when REV ≥ 5 |
| `sm: service "pctl"` | the game state asks for `CheckFreeCommunicationPermission` | `ov_dktf.c`: returns true |
| `movie: no original code for …RegisterClientEvents` | the SDK 4.4 builds `movie::Decoder` with 3 arguments (no trailing int) | `ov_movie.c`: override of the 3-argument constructor |
| Title in Japanese | the game searches for the desired language among `LanguageCode::Make(i)` for i < `GetAvailableLanguageCodeCount`; we answered 1 | `set` returns the full list (15 / 18) |
| Black stripes, missing objects | a 300 MiB CPU-cached pool with no GPU buffer: udmabuf limits each dma-buf to 64 MiB and amdgpu does not import memfd as a host pointer; indices had no backing | `vk_memory.c`: a pool over 64 MiB becomes a sparse buffer bound to 5 udmabufs (requires `sparseBinding`) |
| SIGSEGV in the screen-dimming utility | a lifted inactivity-detection query reads an applet object that never exists | `ov_dktf.c`: returns 0 |
| SIGSEGV in the codec allocation (first cutscene) | the SDK 4.4 AAC codec path allocates through the heap object, which returns NULL without the game's allocator after a static pool | `ov_gfx.c`: `nv::SetGraphicsAllocator` also reaches the SDK's allocator (flag `nv_allocator` in `boot.c`) |

Further fixes (after the title screen):

| Failure | Cause | Fix |
|---|---|---|
| Cutscene frozen on frame 1 | the game's movie player shows frames in queue order and only when their time exceeds the last one shown; the host decoder stamped each output with the *decode* time of its input (B-frames) | `ov_movie.c`: the output time from openh264 |
| H.264 without SPS/PPS | the SDK 4.4 extractor (Android 5.1 stagefright) gives the `avcc` record, not `csd-0`/`csd-1` | `ov_movie.c`: SPS/PPS converted from `avcc` to Annex B |
| "Error" at the end of a movie | TF sends end-of-stream in a 2 MiB buffer at time 0 | the failure of that entry does not count as an error |
| Audio loops that play only once | before REV8 the wave-buffer record has no `loop_count` (Ryujinx `ToCommon(1)`: -1) | `audren.c` (tf-helper `5026941`) |

Diagnostic tools (in `ov_dktf.c`, all enabled by environment variables): `CB_TF_TRACE` (per-symbol call counter with arguments), `CB_TF_CAMPROBE` (timer and state of the cutscene cameras), `CB_TF_MSGLOG=<s>` (script messages delivered: destination, sender, action and event as fourcc).

### Playable 1-1 Mangrove Cove (2026-11-11, *measured* on a development PC, RX 9070 XT, RADV, with a Returns HD sweep running at the same time)

The 1-1 intro waits for the player to **repeatedly press Y/X** (a star icon with Y/X): holding the button does not count. With the scripted input (Y/X alternated every 120 ms, then running right while jumping):

| Window | Frames | ms mean | p50 | p95 | p99 | max | > 16.9 ms | > 33.4 ms |
|---|---|---|---|---|---|---|---|---|
| 220–280 s (`CB_TF_MEASURE=220,280`, run-61) | 3600 | 16.67 | 16.66 | 17.32 | 18.91 | 26.71 | 548 | 0 |

CPU per frame (all threads): **8.25 ms**; the busiest threads: game thread `g100a p44` 5.09, `guest-main` 1.82, `audren` 0.50, `g100f p29` 0.47, `vk_present` 0.19. The same order as Returns HD on the development PC (7–10 ms), with one dominant game thread.

Measured cost (development PC): openh264 decodes the 720p video in 11–14 ms per frame on average, with peaks of 45–70 ms, on one thread.

### Texture formats (static, *measured* on main 1.0.0)

The texture builder translates the game's texture-format enumeration into GPU formats through an internal table (8 bytes per format, the NVN format in the second word). The six formats used by the game's texture files:

| Game format | NVN format | What it is | Runtime (`vk_formats.c` / `tex_vk_format`) |
|---|---|---|---|
| 0x0c | 0x25 | RGBA8 | `VK_FORMAT_R8G8B8A8_UNORM` |
| 0x14 | 0x42 | RGBA_DXT1 (BC1) | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` |
| 0x18 | 0x44 | RGBA_DXT5 (BC3) | `VK_FORMAT_BC3_UNORM_BLOCK` |
| 0x36 | 0x7a | ASTC 5x4 | ASTC: native or decoded to RGBA8 (`vk_astc.c`) |
| 0x37 | 0x7b | ASTC 5x5 | same |
| 0x3a | 0x7e | ASTC 8x5 | same |

All of them have a path in the runtime; none falls into "unsupported format".

### Graphics thread (`g100a`, 6–8 ms per frame) — profile prof-65 (this thread only, `MP4_PROFILE_THREAD=g100a`)

Samples (1 kHz, including when the thread waits): C library 83 % (66 % waiting on futex, **9 % `sched_yield`**, 3.4 % driver `ioctl`, ~2 % close/fstat/lseek), game code 11 % (the UI middleware and the graphics pool allocator), runtime 3.5 %, Mesa Vulkan driver 1.8 %. The `sched_yield` was the presentation pacing of `nvnWindowAcquireTexture` (1–2 ms of spinning per frame); commit `af22bfd` replaces it with a single sleep (A/B comparison pending). The pools that grew by about one per frame (830 → 2037 over 1200 frames) were a leak caused by the runtime, not allocator churn: `nvnTextureGetZCullStorageSize` returned 0 and `SaveZCullInformation` created an empty pool every frame (commit `2c1b37b`, verified in run-72; see `docs/OPTIMIZACION.md`). The close/fstat/lseek calls are not yet explained (those empty pools did not open file descriptors); to be measured with a new profile.

### 1-1 with `CB_SKIP_MOVIES` (2026-11-12, *measured*, run-73, headless 1920x1080)

The first cutscene (149 s) is skipped to 0.0 s; "measuring" starts 19.0 s after the warp; the airplane intro with the Y/X prompt is visible. 5 900 frames: 16.67 ms mean, p99 21.24, 3 frames > 33.4 ms; CPU 9.67 ms per frame (game thread 4.29).

## Still to measure

- Exact format of the `MaterialArchive.arc` file (in MP4: precompiled shaders plus DXBC, zstd-compressed).
- Level 1-1: frame time in ms, CPU per thread, load.
