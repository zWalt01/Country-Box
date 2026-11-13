# System fonts (open)

Country-Box neither uses nor ships Nintendo's fonts. When a game asks for a "system font" (`nn::pl`
`GetSharedFontAddress/Size/LoadState`), the host answers with open fonts (SIL OFL 1.1):

| Type the game asks for | Font | File in `fonts/` |
|---|---|---|
| Standard (Latin, Japanese) | BIZ UDPGothic Regular (Morisawa) — the closest open relative of the console UI's UD gothic | `BIZUDPGothic-Regular.ttf` |
| Simplified Chinese (and its extension) | Noto Sans SC Regular | `NotoSansSC-Regular.ttf` |
| Traditional Chinese | Noto Sans TC Regular | `NotoSansTC-Regular.ttf` |
| Korean | Noto Sans KR Regular | `NotoSansKR-Regular.ttf` |
| NintendoExtension (button glyphs) | no open equivalent: the Standard font is served | — |

## Measured (2026-11-12)

- **Tropical Freeze**: no `nn::pl` imports in any of its modules, so it cannot ask for fonts. An English run (75 s):
  0 calls.
- **Returns HD**: 3 imports in `main` (`GetSharedFontLoadState/Address/Size`). Runs with a build that logged every call,
  in English (90 s, including the boot video) and in `zh-Hans` with `CB_SKIP_MOVIES=1` (100 s, past the title): **0
  calls**. The game draws its text with the fonts in its own content; the 3 imports are most likely Unity's fallback
  for glyphs none of its fonts cover. That is why the fonts are optional.

## How they are obtained and where they live

They are not in the repository. `core/tools/fetch_fonts.py` downloads them from `github.com/google/fonts` at a pinned
commit, checks the SHA-256 pinned in the script, cuts the Regular instance (wght 400) out of the variable Noto fonts
(their default instance is Thin; needs `fonttools`) and puts each `OFL-<family>.txt` next to the `.ttf` files.
`core/tools/build_linux.py` copies that folder to `<build>/fonts/` next to the binary (`--fonts DIR`; by default
`$CB_FONTS_CACHE` or `~/.cache/country-box/fonts`). It is optional: without fonts the build says so and goes on.

## At run time

For each type, the first that exists: `CB_SYSTEM_FONT=<.ttf/.otf/.ttc file>` (that file for every type; a `.ttc` gives
its face 0), `<fonts folder>/<the type's file>`, the Standard file of that folder. The folder is `CB_FONTS_DIR`, or
`fonts/` next to the executable. `Address` is the start of a plain sfnt (as libnx hands it out) and `Size` its length;
`LoadState` is always Loaded. Each type logs a line `pl: font type N (...) served from <file>`.

With no font at all, a line `pl: no system font for type N ...` says where it looked and how to add one, and the game
gets Address 0 / Size 0 for that type (not fatal); text drawn with the game's own fonts is not affected.

The host's own interface (the ImGui menu) uses the bundled Roboto and, for language names, whatever Linux system fonts
it finds; it depends on nothing from Nintendo.
