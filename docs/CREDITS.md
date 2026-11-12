# Créditos

Country-Box se apoya en el trabajo de otros. Sin ellos no existiría.

## Núcleo de recompilación (`core/`)

- **Metroid Prime 4 Recomp** — Jeff Aigner, MIT —
  https://github.com/jaigner-hub/metroid-prime-4-recomp @ `6c9c590`. El lifter ARM64→C, el host que
  sustituye a Horizon OS, el renderer NVN→Vulkan, el video `nn::movie` y el método de verificación
  (oráculo de jump tables, pruebas de funciones) de los que parte Country-Box.
- **fe3h-pc-recomp** — Jeff Aigner, MIT — https://github.com/jaigner-hub/fe3h-pc-recomp
- **totk-pc-recomp** — Valiantsin Dzerakh, MIT — https://github.com/valentderah/totk-pc-recomp
- **LAS Recomp** — Peiikai, MIT — https://github.com/peiikai/LASRecomp (oráculo de jump tables,
  linaje común de la familia).
- Lista completa de componentes, commits y licencias: `core/THIRD_PARTY_NOTICES.md`.

## Launcher (`launcher/`)

- **Dear ImGui** 1.91.9 — Omar Cornut, MIT — https://github.com/ocornut/imgui: el núcleo es el de
  `core/runtime/third_party/imgui`; los backends `imgui_impl_sdl3` e `imgui_impl_sdlrenderer3` van sin cambios en
  `launcher/third_party/imgui_backends/`.
- **SDL 3** — zlib (biblioteca del sistema). **Roboto** — Google, Apache-2.0 (`core/runtime/ui/fonts/`).
- **Open Nectar** — SSunnKing, CC0: sólo ideas de diseño del hub (ver `docs/LAUNCHER.md`); no se copió código.
- El arte del launcher (barril CB, plátanos, palmeras) es propio, dibujado en código.

## Fuentes abiertas (no están en el repo; `core/tools/fetch_fonts.py` las baja al empaquetar)

- **BIZ UDPGothic** — Morisawa Inc., SIL OFL 1.1 — https://github.com/google/fonts/tree/main/ofl/bizudpgothic
- **Noto Sans SC / TC / KR** — Google LLC / Adobe (Source Han Sans), SIL OFL 1.1 —
  https://github.com/google/fonts/tree/main/ofl/notosanssc (y `notosanstc`, `notosanskr`)
- Detalle y uso: `docs/FUENTES.md`.

## Referencias leídas (no copiadas salvo que se anote aquí)

- **PrimeRecomp** — gminustwo, MIT — https://github.com/gminustwo/primerecomp: capa Linux
  (`runtime/platform/linux.c`) de referencia para el port.
- **Savage Wind Recomp** — Revivedx, MIT; **COGBox** — Shagg, MIT; **ProjectCane** — theclub654:
  inspiración del concepto (caja, setup local, AppImage para SteamOS).
- **Cpp2IL** — Sam Byass, MIT; **Il2CppDumper** — Perfare, MIT: herramientas de análisis usadas
  fuera del repo.

El juego, sus archivos y su código son de sus dueños (Nintendo, Retro Studios, Forever
Entertainment). Country-Box no incluye ninguno.
