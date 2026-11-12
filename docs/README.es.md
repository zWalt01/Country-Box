<div align="center">

# Country-Box

**Ports nativos para Linux y Steam Deck de *Donkey Kong Country Returns HD* y *Donkey Kong Country: Tropical Freeze*,
compilados en tu computadora a partir de tu propia copia.**

[English](../README.md) · [Sobre el proyecto](ABOUT.es.md) · [Instalar](INSTALL.md) · [Instalar con un agente de IA](../AGENT-INSTALL.md) ·
[Cómo se probó](TESTING.md) · [Problemas conocidos](KNOWN-ISSUES.md)

![El launcher de Country-Box](images/launcher.webp)

*El launcher: su arte es propio, dibujado en código. Por respeto a los dueños de los juegos, este repositorio no
muestra capturas ni video de los juegos.*

</div>

Quería jugar estos dos juegos de forma nativa en mi Steam Deck, fáciles de instalar y fluidos al jugar, y no encontré
ninguna recompilación de ninguno. Así que hice una. Country-Box es una **recompilación estática**: el código ARM64 del
juego de Switch se convierte a C, se compila para x86-64 y se enlaza con un runtime que reemplaza el sistema de la
consola y dibuja sus gráficos con Vulkan. El resultado es un ejecutable nativo de Linux.

> **Country-Box no incluye nada del juego.** Tú pones tu propia copia (la versión 1.0.0 de Nintendo Switch de cada juego, sin actualizaciones aplicadas,
> extraída en carpetas `exefs/` + `romfs/`). El launcher
> la verifica archivo por archivo, compila el juego para tu computadora y lo ejecuta. Nada aquí extrae ni descifra
> nada, y este proyecto nunca dice dónde conseguir un juego.

## Estado — versión 1.0.0

| | Donkey Kong Country Returns HD | Donkey Kong Country: Tropical Freeze |
|---|---|---|
| Jugable | ✅ juego completo, PC y Steam Deck (barrido con Súper Guía) | ✅ jugado en PC y Steam Deck (sin barrido completo) |
| Fluidez | 60 fps de media en los **81 niveles**, PC y Steam Deck; 99.97 % (PC) y 99.92 % (Deck) de los frames sin tirones | 60 fps de media en los tres niveles medidos |
| Shaders preparados antes de jugar | ✅ 0 compilados al jugar (5 niveles, caché vacío) | ⚠️ ~30 se traducen aún en cada carga de nivel ([problema conocido](KNOWN-ISSUES.md)) |
| 16:10 nativo (pantalla de la Deck) | — (16:9) | ✅ (las cinemáticas de video salen estiradas) |
| Steam Deck (OLED) | los 81 niveles a 60 fps con la APU a **8–9 W** | jugado en la Deck |

Todas las cifras están medidas (2026-11-11/12) y su fuente está en [TESTING.md](TESTING.md) (en inglés); un
"tirón" es un frame que se queda en pantalla más de dos refrescos. Returns HD se
recorrió completo con la propia **Súper Guía** del juego, nivel por nivel, no a mano.

## Instalar

1. Baja el AppImage desde [Releases](https://github.com/zWalt01/Country-Box/releases) y dale permiso de ejecución.
2. Ábrelo, elige **Add your copy** y señala la carpeta de tu juego extraído.
3. Pulsa **PREPARE**. El launcher prepara lo que necesita, convierte y compila el juego para tu computadora y prepara
   los shaders. Si falta una herramienta del sistema, te da el comando exacto para tu distribución.
4. Pulsa **PLAY**.

| Primera compilación (Ryzen 7 5700X3D, 16 hilos) | Returns HD | Tropical Freeze |
|---|---|---|
| Primera vez | **9 min 21 s** | **2 min 31 s** |
| Tras una actualización | segundos: sólo se recompila lo que cambió | 10 s |

**Steam Deck:** SteamOS no trae compilador. Compila en una PC con Linux y usa **Import a build** en la Deck.
**Con un agente de IA:** dale [AGENT-INSTALL.md](../AGENT-INSTALL.md): maneja la línea de comandos del launcher, que
responde en JSON. Requisitos, compilación manual y pasos de la Deck: [INSTALL.md](INSTALL.md).

**Requiere** un CPU x86 de 64 bits con AVX2 (Intel Haswell / AMD Zen o más nuevo, Steam Deck) y una GPU con Vulkan.

## Sobre el proyecto

Soy Walter, estudiante de ciencia de datos, y quería saber qué tan difícil es de verdad un proyecto así. Lo construí
dirigiendo agentes de IA y comprobando cada resultado con corridas reales; la historia completa está en
[ABOUT.es.md](ABOUT.es.md). Dudas y errores: [Issues](https://github.com/zWalt01/Country-Box/issues).

## Créditos

El núcleo de recompilación viene de **[Metroid Prime 4 Recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp)**
de Jeff Aigner (MIT, commit `6c9c590`), que trae el linaje de fe3h-pc-recomp, totk-pc-recomp, LAS Recomp y otros; se
usa con crédito completo como base de un proyecto nuevo, no como fork. Cada componente, con su commit y licencia:
[core/THIRD_PARTY_NOTICES.md](../core/THIRD_PARTY_NOTICES.md); resumen: [CREDITS.md](../CREDITS.md). Fuentes: BIZ
UDPGothic y Noto Sans (SIL OFL), descargadas al compilar.

## Licencia

MIT — ver [LICENSE](../LICENSE). `core/` conserva su licencia MIT original y los encabezados de cada archivo.

## Legal

*Donkey Kong Country Returns HD* y *Donkey Kong Country: Tropical Freeze* son propiedad de Nintendo / Retro Studios /
Forever Entertainment. Country-Box es un proyecto independiente y sin fines de lucro, sin relación con ellos ni su
respaldo. No contiene código, recursos, llaves ni firmware de los juegos. Necesitas tu propia copia obtenida legalmente. Declaración completa:
[LEGAL.es.md](LEGAL.es.md).
