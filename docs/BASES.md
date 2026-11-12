# Adjacent foundations — static Switch recompilations (PLAN step 3)

Status: **first pass 2026-11-10**. Source of each row: a `--depth 1` clone in a temporary reference folder (commit indicated) + its README/docs. Pending: LAS Recomp, Savage Wind (read), Power Moon (local fork), TotK.

## Families

1. **LAS Recomp** (peiikai, MIT): LAS → Power Moon (Odyssey), Savage Wind (BOTW), **PrimeRecomp**. C runtime, Horizon kernel/services, **NVN→Vulkan**, Linux + Deck + AppImage.
2. **totk-pc-recomp** (valentderah, MIT): TotK → fe3h-pc-recomp → **jaigner-hub/metroid-prime-4-recomp**. Project explicitly designed as a *shared core* that game N+1 receives for free. NVN→Vulkan, Windows.
3. **SwitchRecomp** (MZ Labs, AGPL-3.0 + mandatory splash screen): generic → **linuxopt/mp4-recomp**. NVN→OpenGL 4.6, native Linux AppImage. It is the one that already boots Returns HD here.

## Matrix

| Project | Game / engine | License | Linux | GPU | `nn::movie` video | Commit |
|---|---|---|---|---|---|---|
| gminustwo/primerecomp | Prime Remastered / **Retro** | MIT | yes + Deck | Vulkan | ? | `3fd9da0` |
| jaigner-hub/metroid-prime-4-recomp | Prime 4 Beyond / **Retro** (`GameFramework`, RS15, SDK 19.3) | MIT | no (Windows) | Vulkan | **yes**, H.264 + AAC (`ov_movie.c`, OpenH264) | `6c9c590` |
| linuxopt/mp4-recomp | Prime 4 Beyond / **Retro** | GPL-3.0 (SwitchRecomp fork) | **yes, AppImage**; tested on **RX 9070 XT** + Mesa | OpenGL 4.6 | **yes** (OpenH264 or libavcodec, VA-API) | `64c1f21` |
| SwitchRecomp v190 (local) | generic | AGPL-3.0 + splash | yes | OpenGL (Vulkan Windows only) | no without optional OpenH264 | `713c861` |
| Revivedx/SavageWindRecomp | BOTW / Nintendo | MIT | yes + Deck | Vulkan | — | `d8c5e70` |

## What carries over to our games (measured)

- **Video**: DKCRHD, DKCTF and MP4 use the same `subsdk0` = `NintendoSDK_movie`. The two MP4 recompilations already implement it → a direct route for the Returns HD stall.
- **Retro engine**: DKCTF `main` = `GameFramework.nss` from `full_rs12`; MP4 = `GameFramework` RS15; both with `RFRM` and `MaterialArchive.arc`. Large difference: SDK 4.4.0 (DKCTF) vs 19.3 (MP4).
- **Unity**: no recompilation found so far runs a Unity game; SwitchRecomp is the only one that has already booted Returns HD (86 frames).

## Test 1 (2026-11-10): totk lifter on Returns HD

0 unsupported instructions in the three modules, a jump-table oracle with the same kind of residue as MP4, and the generated C compiles entirely with clang on Linux (544/544, 0 errors). Details in [games/DKCRHD.md](games/DKCRHD.md). Conclusion: the lifter is not the obstacle; the runtime is, since it is Windows-only (65 of 141 files touch Win32/D3D). The Linux port takes from the LAS family.

## Out of scope

- *Mario Kart 7* (3DS) and the Wii version of *Returns* (2010): Retro, but not Switch; Returns HD is a Unity remake that shares no code with the original.
