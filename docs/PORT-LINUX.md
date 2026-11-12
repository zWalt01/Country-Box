# Linux port of the runtime (core from totk / jaigner-hub MP4 @ `6c9c590`)

Status: **title screen rendered on Linux** (2026-11-11, measured on a development PC). `--boot` passes 5/5. The first milestone is `--boot` on Linux: Returns HD runs from `nn::init::Start` until its first call to `nvnBootstrapLoader`, without a window.

## What was measured

- The core already has a platform layer: `runtime/platform/platform.h` (132 lines, 50 `plat_*` functions covering threads, mutex/condvar, sleep, memory, files, environment, dynamic libraries, CPU/GPU load and child processes) with **one** implementation, `platform/win32.c` (698 lines).
- `tools/check_platform.py` is the authoritative list of what still calls Win32 directly: **1 674 calls in 41 files**. The largest are `fs_host.c` 177, `vk_pipeline.c` 157, `vk_memory.c` 137, `romfs.c` 95, `memory.c` 90, `kernel.c` 83, `gfx_shader.c` 83, `gfx_window.c` 79, `vk_backend.c` 75, `vk_sync.c` 65, `ov_system.c` 65, `prof_win.c` 56 and `input_win.c` 56.
- SDL3 is already used for controllers, sensors, rumble and audio (`audio_mixer.c`, `input_win.c`, `ov_input.c`).
- MIT-licensed Linux reference: `gminustwo/primerecomp`, `runtime/platform/linux.c` (347 lines, LAS family). It reserves the guest address space with `mmap(MAP_FIXED_NOREPLACE)`, uses a shared heap backed by `memfd_create`, uses **`udmabuf`** so the driver can import guest memory without a copy (noted there as required on the Steam Deck), and tracks writes with **`userfaultfd`** (the equivalent of `MEM_WRITE_WATCH`). It also includes `posix.c`/`posix.h`.
- Note: PrimeRecomp's Steam Deck preset targets 30 fps. Our target is 16.6 ms per frame; this is measured, not assumed.

## Phases (each with a verifiable criterion)

1. **`platform/linux.c`**: implement the 50 functions of `platform.h` with POSIX/Linux, taking what applies from `primerecomp/runtime/platform/{linux,posix}.c` (MIT, attributed in `THIRD_PARTY_NOTICES.md`). Criterion: the platform/kernel tests (`--probe-kernel`) pass on Linux.
2. **Guest memory (2.7a)**: `memory.c`, `loader.c`, `vk_memory.c` and parts of `kernel.c`. `VirtualAlloc/Protect` maps to `mmap/mprotect`, vectored exceptions map to `sigaction(SIGSEGV)`, write-watch maps to `userfaultfd`, and Vulkan import uses `udmabuf`. This is the most performance-sensitive part on the Steam Deck. Criterion: `--probe-loader` loads the four Returns HD modules at their addresses.
3. **Threads, locks, time (2.7b) and "pending"**: SRW/CRITICAL_SECTION/Interlocked/GetTickCount in `kernel.c`, `svc.c`, `dispatch.c`, `gfx_shader.c`, `vk_*`, `ov_*`, `romfs.c` and `fs_host.c` map to `plat_*` or C11 atomics. This is mechanical and done file by file, using `check_platform.py` as a counter. Criterion: `check_platform.py` only reports the permanent entries (window/input/audio/probe).
4. **`--boot` of Returns HD on Linux** (no window). Milestone criterion.
5. **Window, input, audio**: `gfx_window.c` moves to SDL3 plus a Vulkan surface (X11/Wayland; Gamescope on the Steam Deck), `input_win.c` to SDL3 (nearly done already), and `audren.c` to SDL3 audio. Criterion: first frame presented.
6. **Measurement on the Steam Deck from the first frame**: `perfmon.c` plus `plat_cpu_load`/`plat_gpu_load` on Linux (`/proc/self/task/*/stat`, `/sys/class/drm/card*/device/gpu_busy_percent`), reporting mean/p99/peak frame time per scene, compatible with MangoHud. Criterion: a report with those fields taken on the Steam Deck.
7. **Linux build**: `tools/build_game.py` assumes MSYS2/`.exe`; a Linux version uses clang + lld + ninja, the system SDL3 and Vulkan, and then a glibc 2.35 container for the AppImage.

## Progress

### Phase 1 — `platform/linux.c`: done (2026-11-10)

- All 50 functions of `platform.h`. Locks and condvars use **futex** (8 bytes, zero is valid, like Windows SRW; `pthread_mutex_t` does not fit). Threads use a 16 MiB stack, matching what the Windows linker gives with `--stack`. Sleep uses an absolute `clock_nanosleep` with 1 µs slack and 50 µs of spin (kept short on purpose, since spinning costs battery on the Steam Deck). Machine load is read from `/proc` and sysfs (amdgpu `gpu_busy_percent`, `mem_info_vram_used`; k10temp/zenpower/coretemp/thermal_zone).
- Test: `core/tests/platform_check.c`: **257 checks, 0 failures**, with clang and gcc, 10/10 runs. Measured on a development PC: uncontended mutex 4.1 ns, contended (8 threads) 75 ns; sleep 0.1–16.7 ms with median overshoot 0.5 µs and maximum 1.2 µs. To be repeated on the Steam Deck.

### Phase 2 — guest memory (`memory.c`): done (2026-11-10)

- Reservations use `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)`; commit is `mprotect` to RW; decommit punches a hole in the memfd (`FALLOC_FL_PUNCH_HOLE`) or uses `MADV_DONTNEED`, so pages return as zeros and release RAM.
- Watched regions (the 6 GiB stack+heap block, the 64 GiB alias and pool shadows) are **backed by memfd**, so `mem_dmabuf()` exports them as **udmabuf** and Vulkan imports them as ordinary GPU memory. Reason, measured by PrimeRecomp on the Steam Deck: importing the heap as a host pointer (userptr) makes amdgpu walk every page on every submit, about 5 ms per 3.3 GB, three times per frame.
- Write tracking uses `userfaultfd` (user mode, allowed with `unprivileged_userfaultfd=0`) with asynchronous write-protect and `PAGEMAP_SCAN` (Linux 6.7+; the Steam Deck ships 6.11). Windows "frozen" regions are not used: `mem_freeze` returns 0, and `vk_memory.c` still queries it.
- Horizon's `R_OK` (0) conflicts with `R_OK` from `unistd.h` (4), so it is explicitly `#undef`-ed.
- Test: `core/tests/memory_check.c`: **31 checks, 0 failures** (heap grows and shrinks with zeros, exact tracking of written pages, peek without reset, udmabuf, aliases, stack movement, QueryMemory, kernel allocator, shadows). Measured on a development PC: a 64 MiB query with no writes takes **38 µs**. To be repeated on the Steam Deck.

### Phase 3 — kernel: **`--probe-kernel` passes on Linux** (2026-11-10)

Ported to Linux: `kernel.c`, `loader.c`, `svc.c`, `dispatch.c`, `hostfn.c`, `cfg.c`, `kernel_probe.c` and `cfg_probe.c`. New `rt_base.c` (logging, traces, a SHA-256 implementation, configuration, break hooks, `rt_brk`/`rt_udf`, `rt_ticks`), moved out of `main.c` so that probes and the game link the same code. VMA 3.4.0 is vendored in `runtime/third_party/vma/` (same version as upstream). The conversion is mechanical (SRW, `Interlocked*` with `LONG`→`int32_t`, `_aligned_*`, `_strdup`, ...) using a one-off script, with the diff reviewed; the rest was done by hand.

The probe is built by `core/tools/build_probe.sh`, which links `tests/kernel_probe_main.c` (starts `main()`; stubs call `rt_fatal` if reached, except the no-ops on the ExitProcess path), the ported files, and the 544 lifted objects of Returns HD compiled with `-mno-red-zone`. The binary is 314 MB.

Result (measured on a development PC, **10/10 runs `probe-kernel: passed`**, exit 0): priority lock handoff, condvar ordering (the negative control fails as it should), address arbiter, events with 5.01/10.01 ms timeouts, virtual cores (8 threads × 300, at most 3 at a time, 0 collisions), **10 000 fiber switches at 0.055–0.094 µs each**, 0 registers altered, ExitThread from host calls nested inside host calls and from a fiber, and **real lifted Returns HD code**: 4 threads × 1000 calls into an SDK time function, monotonic and within the `rt_ticks` window. 5 056 SVCs, 33 threads.

**Asynchronous signal-based preemption** (our own test, `test_async_preempt`, 2026-11-10): three p59 threads fill the 3 virtual cores with a spin loop (`runtime/kernel_probe_spin.c`: a loop with no SVC or call, state held in general-purpose and YMM registers, compiled like the lifted code and linked *between* two lifted objects, at host depth 0). The main thread (p44) wakes from a sleep and starts a p28 thread. *Measured* on a development PC, 10/10 runs: main waits **0.29–0.30 ms** for a core, the p28 thread **1.29–1.31 ms**, no spinner had finished (each runs ~290 ms), 3 interrupts sent / 3 cores yielded / 0 rejected, and the spinners' results are identical to the run without interrupts (registers preserved). Performance note: the 1.3 ms for p28 is `AP_FIRST_NS` (0.25 ms) plus `AP_GAP_NS` (1 ms, the same owner is not interrupted again that soon). On the Steam Deck, with 16.7/33.3 ms frames, it is worth measuring in-game before changing it.

Per-game constants inherited from the earlier port must move to a profile: `PROGRAM_ID` in `svc.c` belongs to TotK, and `MAIN_STACK`/`MAIN_PRIO`/`MAIN_CORE` in `kernel.c` come from the MP4 `main.npdm`.

`kernel.c` has about 100 Win32 call sites. Two mechanisms are not mechanical:

- **Fibers** (guest `nn::os` fibers on top of Windows fibers with `FIBER_FLAG_FLOAT_SWITCH`): on Linux, a custom x86-64 context switch in assembly (callee-saved registers, MXCSR and the x87 control word). Not `swapcontext`, which makes a system call (`sigprocmask`) on every switch.
- **Asynchronous preemption of virtual cores** (`ap_try`, which suspends the owning thread, reads its context and moves its instruction pointer to `kern_ap_stub`): on Linux, a real-time signal is sent to the owning thread; the handler, running on that thread, checks the instruction pointer in the `ucontext` and redirects it. `kern_ap_stub` is rewritten for the **System V** ABI: it also saves `rdi`/`rsi` (volatile under SysV), with no shadow space and no `.seh` directives.
- **Red zone**: the System V ABI lets leaf functions use 128 bytes below `rsp` without moving it, and the stub pushing onto that stack would overwrite them. The solution is to compile **lifted code with `-mno-red-zone`** (only lifted code can be interrupted; the kernel checks that the instruction pointer is inside its range). The cost is negligible.
- Sampling the stack of another thread (diagnostics `MP4_CORE_DIAG` / `MP4_THREAD_DUMP`): the same signal, returning the instruction and stack pointers; a full unwind is left for later.

## Out of scope for this plan

Resolving the computed branches in `main` and the Unity `dt` value are tracked separately and are verified when the game runs.

### `--boot` milestone (2026-11-11): **passes 5/5 on a development PC**

Returns HD runs from `nn::init::Start` to its first call to `nvnBootstrapLoader`: the three DT_INIT sections (310/45/94 entries), nnMain, RomFS mounted from the directory, RAM disk, save, controllers, about 620 SVCs and 25 threads. *Measured*. Beyond the per-file port, the following were needed:

- The `HOST_OVERRIDE` tables relied on the PE/COFF `$` section trick; on ELF no override was installed. They now use `rt_ovr`/`rt_obs` sections with `__start_`/`__stop_`.
- CMIF domains in `ipc.c` (SDK 18.3.2 converts sessions to domains) and the `lm` service.
- Symbol resolution as Horizon's rtld does it: the first module in load order that exports the name. The Returns HD main module exports the Unity allocator functions (`malloc`/`free`/`operator new`/`delete`); with the old rule ("own definition first") the SDK freed Unity memory from its own heap.
- Thread suspension (`SetThreadActivity`/`GetThreadContext3`) for the Unity garbage collector: a paused thread stops at an SVC or indirect call, where the lifted context is complete.
- `ov_dkcr.c`: Returns HD-specific overrides (global filesystem cache, `ro`, offline network: ssl, socket, nifm), and in `fs_host.c` the RAM disk, `IsMounted` and directory operations.

### First-frame milestone (2026-11-11): **title screen**

`--seconds 30 --shot` produces the Returns HD title screen (1920x1080) on a development PC, with 0 validation errors and 0 shader failures. *Measured* on a development PC (RX 9070 XT, RADV, Wayland): 53 fps mean, 1 % low 44 fps. What was needed, in order of appearance:

- Anonymous guest memory: amdgpu/RADV only imports anonymous host pointers (memfd gives `INVALID_EXTERNAL_HANDLE`).
- `audout:u` (IAudioOut on an SDL3 stream), `nvnTexturePoolRegisterImage`, and the builder's stencil mode.
- Public HID of SDK 18.3.2 (the SDK calls internal `detail::` functions through direct branches that no override intercepts).
- **C++ exceptions**: `unw_resume` is a host hook (`HOOK_SYMBOLS` in the generator) that reads the context from the cursor and marks a non-local resume; each lifted call checks the mark on return, and the function whose stack pointer matches jumps to its landing pad, while the others return. This requires regenerating the C code.
- QueryMemory: anything that is not part of the game is reported as inaccessible (the unwinder was walking into "Code RX" outside the modules).
- `nvnProgramSetShaders` copies the control sections (Unity frees them after the call).
- Textures with a separate sampler (a packed handle in the style of Ryujinx's SeparateSamplerHandle).

Performance finding: with userptr imports, precise write tracking (without THP, with write-protect when committing) drops the title from 53 to 32 fps. It remains optional (`CB_PRECISE_WRITE_WATCH=1`). Importing through udmabuf is the fix (see the tracked items).

### Playable milestone (2026-11-11): **level 1-1 with input**

With scripted input (`MP4_INPUT_SCRIPT`, script in the local build directory `<build dir>`), Returns HD on a development PC goes from title to Select Game, Start Game, the intro, the island map (free look, pause, world selection), and level 1-1 Jungle Hijinxs: DK leaves the house by repeating X/Y (the Wii "shake" motion), hits the Tiki, walks, jumps and rolls, with the heart HUD. What was needed: `nn::oe` Enter/LeaveExitRequestHandlingSection, `SetPerformanceConfiguration(int)`, `GetPseudoDeviceId`.

*Measured* on a development PC (RX 9070 XT, RADV): the island map and the level run at about 20 fps mean (1 % low 8–9 fps); replication takes 45–60 ms and texture uploads about 16 ms per frame at worst. This is the next focus.

Intermittent failure seen once in 4 runs, when loading the map: `radv/amdgpu: The CS has been rejected (-14)` (EFAULT) after a burst of `UnmapPhysicalMemory`. Hypothesis (*assumed*): a live userptr import covers pages that the decommit left as `PROT_NONE`. `memory.c` no longer protects them when they overlap a live import (it logs this); the next run showed no overlap and no failure, so the cause is not confirmed.

### Performance patch #1 (2026-11-11): **udmabuf import — level at 60 fps on a development PC**

*Measured* with `MP4_TRACE=perf`: on the map and the level, five DXT5 textures of 4096² (16 MiB each) were re-uploaded every frame (529 consecutive frames; up to 4.7 GiB across 60 frames), with the GPU taking 0.5–1 ms per frame (`bound: cpu`). The test `core/tests/userptr_wp_check.c` isolated the cause: with a host-pointer import (amdgpu userptr) and tracking through userfaultfd WP_ASYNC plus PAGEMAP_SCAN, every submit that uses the import after re-protecting reports all 8192 pages as written by the CPU (with and without THP). With a udmabuf dma-buf over a memfd: 0 pages without writes, 1 with a real write, coherent between CPU and GPU in both directions.

Change: guest memory is memfd by default when `/dev/udmabuf` opens; `vk_memory.c` imports pools and 64 MiB arenas as dma-buf (`VK_EXT_external_memory_dma_buf`), with a host pointer as fallback; decommit over a live import writes zeros instead of punching a hole (udmabuf pins the pages).

| Scene (development PC, RX 9070 XT) | Before (userptr) | After (udmabuf) |
|---|---|---|
| Title | 53 fps (1 % low 44) | 59.2 fps (1 % low 48.8) |
| Map + level 1-1 | ~20 fps (1 % low 8–9) | 59.8 fps (1 % low 49.4) |
| Texture re-uploads | 660–4 158 per 60 frames | 0 |

`--boot` passes. The figures were taken with two builds run by parallel workers (load 6–9).
