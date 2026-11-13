# Credits

Country-Box stands on other people's work. It would not exist without them.

## Recompilation core (`core/`)

- **Metroid Prime 4 Recomp** — Jeff Aigner, MIT —
  https://github.com/jaigner-hub/metroid-prime-4-recomp @ `6c9c590`. The ARM64→C lifter, the host that replaces
  Horizon OS, the NVN→Vulkan renderer, `nn::movie` video and the verification method (jump-table oracle, function
  tests) that Country-Box starts from.
- **fe3h-pc-recomp** — Jeff Aigner, MIT — https://github.com/jaigner-hub/fe3h-pc-recomp
- **totk-pc-recomp** — Valiantsin Dzerakh, MIT — https://github.com/valentderah/totk-pc-recomp
- **LAS Recomp** — Peiikai, MIT — https://github.com/peiikai/LASRecomp (jump-table oracle, the family's shared
  lineage).
- Full list of components, commits and licences: `core/THIRD_PARTY_NOTICES.md`.

## Launcher (`launcher/`)

- **Dear ImGui** 1.91.9 — Omar Cornut, MIT — https://github.com/ocornut/imgui: the core is the one in
  `core/runtime/third_party/imgui`; the `imgui_impl_sdl3` and `imgui_impl_sdlrenderer3` backends are unchanged in
  `launcher/third_party/imgui_backends/`.
- **SDL 3** — zlib. **Roboto** — Google, Apache-2.0 (`core/runtime/ui/fonts/`).
- **Open Nectar** — SSunnKing, CC0: design ideas for the hub only; no code was copied.
- The launcher's art (the CB barrel, bananas, palm trees) is our own, drawn in code.

## Open fonts (not in the repository; `core/tools/fetch_fonts.py` downloads them when packaging)

- **BIZ UDPGothic** — Morisawa Inc., SIL OFL 1.1 — https://github.com/google/fonts/tree/main/ofl/bizudpgothic
- **Noto Sans SC / TC / KR** — Google LLC / Adobe (Source Han Sans), SIL OFL 1.1 —
  https://github.com/google/fonts/tree/main/ofl/notosanssc (and `notosanstc`, `notosanskr`)
- Details and use: [FONTS.md](FONTS.md).

## References read (not copied unless noted here)

- **PrimeRecomp** — gminustwo, MIT — https://github.com/gminustwo/primerecomp: the Linux layer
  (`runtime/platform/linux.c`) used as a reference for the port.
- **Savage Wind Recomp** — Revivedx, MIT; **COGBox** — Shagg, MIT; **ProjectCane** — theclub654: inspiration for the
  concept (the box, local setup, an AppImage for SteamOS).
- **Cpp2IL** — Sam Byass, MIT; **Il2CppDumper** — Perfare, MIT: analysis tools used outside the repository.

The game, its files and its code belong to their owners (Nintendo, Retro Studios, Forever Entertainment). Country-Box
includes none of them.
