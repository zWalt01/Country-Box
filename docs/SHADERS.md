# Shader preparation before play

As with modern games, shaders are prepared after installation, and the game cannot be played until preparation finishes. Relevant code: `core/runtime/shader_prep.c` (generic), `core/runtime/shader_src_unity.c` (Returns HD), `core/runtime/ui/prep_screen.cpp` (screen). Shader lists: `games/dkcrhd/`.

## What happens

1. At startup (`rt_boot`), before loading the game, the runtime compares the marker `shader-prepared.id` (next to the executable) with what the caches would be today. The inputs are: the translator identity (`mp4_xlate.id` + host responses + key version), the GPU (vendor:device), driver version and info (`driverVersion`, `driverInfo`), `pipelineCacheUUID`, a hash of the three lists, and the driver cache folder. If anything differs (or there is no marker), it shows the blocking **"Preparing shaders"** screen and prepares. A Mesa update on SteamOS changes `driverInfo`/`driverVersion` and triggers preparation again.
2. Preparation: the variant list (`shader-list.bin`, shipped) + the variants recorded by this installation → programs extracted from the user's own game files → seeds → translation on all cores → the pipelines in `pipeline-list.bin` (merged into `pipeline-keys.bin`) → each stage library into the driver cache → `pipeline-cache.bin` → marker.
3. Screen: step N of 4, counter N / M, a total time bar, elapsed and estimated remaining time, and the reason. No game art or typography (the runtime's Roboto, plain text). It takes no input: the game starts on its own when preparation ends. Test capture: `CB_PREP_SHOT=<png>[:<ms>]`.
4. Headless: `country-box --prepare-shaders` (progress on stdout). `CB_SHADER_PREP=0` skips it, `=force` forces it. Runs with `--shot` and `MP4_PIPE_SYNC=1` do not run it (they build synchronous pipelines on purpose).

## Where the programs come from (Returns HD)

*Measured* (2026-11-11, a UnityPy script kept outside the repo and the C scanner `shader_src_unity.c` with a test harness). Unity 2022.3.42f1, files without type trees:
- `Shader` objects (class 48): an LZ4 block blob at the end of the object (`platforms` = [19] NVN, `offsets`, `compressedLengths`, `decompressedLengths`, `compressedBlob`). 16 628 distinct programs (8 314 VS + 8 314 FS).
- `ComputeShader` objects (class 72, in `globalgamemanagers.assets` and `Resources/unity default resources`): 78 compute programs stored in plain form.
- NVN program = a control section (0x880 bytes, magic 0x98761234, code length at +0x704, stage at +0x714: 0 vertex, 1 fragment, 5 compute) followed by the code (magic 0x12345678; compute 0x12345679).
- Total 16 706 programs. `exefs/main`: none.
- Reading: the SerializedFile object table (format 22) is read and only objects 48/72 are touched: **20.3 MB of 3.6 GB**, 90-130 ms on the PC with a warm page cache (16 threads). The full scan (fallback, `CB_SHADER_SCAN=full`) gives the same result in 0.5-1.1 s.

Coverage of what the PC sweep recorded (907 programs at 18:11): **900** come from the game files, **4** more with control-section patches (`games/dkcrhd/shader-patches.bin`: 8-byte fields that the game writes into the control section at runtime, values like `478000003f00xxxx`, meaning not yet known; each patch is verified by hash), and **3** not at all (two with different code, probably c1 constants written at runtime; one with the control section rewritten, magic 0x98761201). Those three only come from recordings made by the installation itself (`shader-programs.bin`, fallback). Preparation writes `shader-patches.found.bin` when it finds new patches.

## Driver cache

Stage libraries live in the driver's on-disk cache. *Measured*: preparation sets `MESA_SHADER_CACHE_DIR=<install>/driver-cache` (and `MESA_SHADER_CACHE_MAX_SIZE=4G`, and `__GL_SHADER_DISK_CACHE_PATH` for NVIDIA) before creating the device; 11 MB after preparing 904 variants. This way another game, or a launch in a different environment (Steam / outside Steam), does not empty it. `CB_DRIVER_CACHE=system` uses the system cache.

## Timings

PC (RX 9070 XT, RADV Mesa 26.2.5, 16 threads; `country-box --prepare-shaders` in an empty folder, *measured*): 1.6-2.2 s in total — scan 0.1 s, 904 translations 0.8-1.2 s, 465 pipelines 0.3 s, 900 stage libraries 0.3 s. With the screen (`--seconds 30` in an empty folder): 2.6 s from the moment it appears until the game starts.

### Sweep with an empty cache (PC, 2026-11-11, *measured*)

A fresh folder with only the binary, the translator, the lists and the profile (`<fresh build dir>`), an empty driver cache of its own; `country-box --prepare-shaders` and then `tools/sweep.py --build <fresh build dir> --out <sweep output dir> --levels 0,13,18,27,48`:

| level | max translation (ms, "worst frame" lines) | max pipeline | background pipelines | new variants | mean / p99 ms | pipeline/shader waits |
|---|---|---|---|---|---|---|
| 0 (K) | 0.0 | 0.0 | 0 | 0 | 16.67 / 18.02 | 0/0 |
| 13 (1-2) | 0.0 | 0.0 | 0 | 0 | 16.67 / 17.90 | 0/0 |
| 18 (boss 1) | 0.0 | 0.0 | 0 | 0 | 16.69 / 18.99 | 0/0 |
| 27 (boss 2) | 0.0 | 0.0 | 0 | 0 | 16.67 / 19.41 | 0/0 |
| 48 (5-5) | 0.0 | 0.0 | 0 | 0 | 16.67 / 18.66 | 0/0 |

`shader-answers.bin` and `pipeline-keys.bin` did not change after preparation: no level requested a variant or a pipeline outside the list. The remaining worst frames (330-380 ms during loading, 206 ms at level 18 without control) have translation and pipeline at 0: they come from loading / game work, not from shaders.

Steam Deck: not measured yet. *Assumed*: scan ~2 s on NVMe, ~40 s if the game is on a microSD card without the object table (with it, 20 MB are read).
