# Fuentes del sistema (abiertas)

Country-Box no usa ni distribuye fuentes de Nintendo. Si un juego pide una "fuente del sistema" (`nn::pl`
`GetSharedFontAddress/Size/LoadState`), el host responde con fuentes abiertas (SIL OFL 1.1):

| Tipo que pide el juego | Fuente | Archivo en `fonts/` |
|---|---|---|
| Standard (latín, japonés) | BIZ UDPGothic Regular (Morisawa) — pariente abierto más cercano del gótico UD de la interfaz de la consola | `BIZUDPGothic-Regular.ttf` |
| Chino simplificado (y su extensión) | Noto Sans SC Regular | `NotoSansSC-Regular.ttf` |
| Chino tradicional | Noto Sans TC Regular | `NotoSansTC-Regular.ttf` |
| Coreano | Noto Sans KR Regular | `NotoSansKR-Regular.ttf` |
| NintendoExtension (glifos de botones) | sin equivalente abierto: se sirve la Standard | — |

## Medido (2026-11-12)

- **Tropical Freeze**: 0 imports de `nn::pl` en todos sus módulos; no puede pedir fuentes. Corrida en inglés (75 s): 0 llamadas.
- **Returns HD**: 3 imports en `main` (`GetSharedFontLoadState/Address/Size`). Corridas con una build
  instrumentada que registraba cada llamada: inglés (90 s, incluye el video de arranque) y `zh-Hans` con
  `CB_SKIP_MOVIES=1` (100 s, hasta después del título): **0 llamadas**. El juego dibuja su texto con las
  fuentes de su propio contenido; los 3 imports son (supuesto) el respaldo de Unity para glifos que ninguna
  fuente suya cubre. Por eso las fuentes son opcionales.

## Cómo se obtienen y dónde viven

No están en el repositorio (el repo público rechaza archivos de más de 1 MiB). `core/tools/fetch_fonts.py` las baja
de `github.com/google/fonts` en un commit fijado, comprueba el SHA-256 fijado en el propio script, corta la instancia
Regular (wght 400) de las Noto variables (su instancia por defecto es Thin; requiere `fonttools`) y deja cada
`OFL-<familia>.txt` junto a los `.ttf`. `core/tools/build_linux.py` copia ese directorio a `<build>/fonts/` junto al
binario (`--fonts DIR`; por defecto `$CB_FONTS_CACHE` o `~/.cache/country-box/fonts`). Es opcional: sin fuentes el
build avisa y sigue.

## En ejecución

Por tipo, el primero que exista: `CB_SYSTEM_FONT=<archivo .ttf/.otf/.ttc>` (sirve ese archivo para todos los tipos;
un `.ttc` entrega su cara 0), `<carpeta de fuentes>/<archivo del tipo>`, el archivo Standard de esa carpeta. La carpeta
es `CB_FONTS_DIR`, o `fonts/` junto al ejecutable. `Address` es el inicio de un sfnt plano (supuesto, como lo entrega
libnx: ninguna corrida lo ha llamado) y `Size` su largo; `LoadState` siempre es Loaded. Cada tipo registra una línea
`pl: font type N (...) served from <archivo>`.

Sin ninguna fuente: una línea `pl: no system font for type N ...` indica dónde buscó y cómo ponerla, y el juego recibe
Address 0 / Size 0 para ese tipo (no es fatal); el texto con las fuentes propias del juego no se afecta. Verificado:
`--probe-services` con y sin carpeta (`work/fonts-04.log`, `fonts-05.log`) y Returns HD sin carpeta (`fonts-07.log`).

La interfaz propia del host (menú ImGui) usa Roboto incluida y, para los nombres de idiomas, fuentes del sistema
Linux que encuentre (`plat_font_path`); no depende de nada de Nintendo.
