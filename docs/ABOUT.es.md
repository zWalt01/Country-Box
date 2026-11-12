# Sobre Country-Box

**[English](../ABOUT.md)**

Hola, soy **Walter**. Estudio ciencia de datos, y Country-Box empezó con una pregunta: *¿qué tan difícil es, de verdad,
hacer uno de estos proyectos de recompilación?*

No encontré ninguna recompilación de estos dos juegos, y las que sí encontré de otros juegos casi nunca estaban
pensadas para la Steam Deck: fáciles de instalar y rápidas una vez que corren. Así que intenté hacer la que yo quería
usar: pones tu propia copia, pulsas un botón y juegas de forma nativa en Linux o en la Deck, a 60 fps estables.

Resultó muy difícil con algunos modelos, en parte porque no los conocía bien, y casi aterradoramente fácil con otros
(Claude Opus 5.5).

## Lo que hice

- Tomé el núcleo de recompilación de [Metroid Prime 4 Recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp)
  (Jeff Aigner, MIT), que a su vez viene de fe3h-pc-recomp, totk-pc-recomp, LAS Recomp y otros, y porté su runtime
  de Windows a Linux y la Steam Deck.
- Hice que *Donkey Kong Country Returns HD* (un juego de Unity) y *Tropical Freeze* (motor propio de Retro) arranquen,
  se jueguen y mantengan 60 fps; Returns HD en sus 81 niveles, en PC y en la Steam Deck.
- Construí la preparación de shaders que corre antes de jugar, para que el juego no se trabe compilando shaders a
  mitad de un nivel (completa en Returns HD; Tropical Freeze aún tiene un hueco conocido).
- Escribí el launcher: verifica tu copia, compila el juego en tu propia computadora, protege tus partidas en las
  actualizaciones y se actualiza solo.
- Medí todo. Cada cifra de este repositorio tiene fecha y fuente en [TESTING.md](TESTING.md); lo que no se midió se
  dice tal cual.

## Cómo se hizo

Construí Country-Box dirigiendo agentes de IA para programar (los modelos Claude de Anthropic y GLM de Z.ai). Yo puse
las metas, tomé las decisiones, jugué los juegos y juzgué los resultados; los agentes escribieron y probaron el código
bajo esa dirección, y revisaron el trabajo de los otros. Cada afirmación se comprobó con una corrida real antes de
escribirla, y cada línea que se publica se revisó antes, incluida una pasada línea por línea en busca de datos
personales y de cualquier cosa que pertenezca a los dueños de los juegos.

Lo digo abiertamente porque creo que es parte de la respuesta a la pregunta con la que empezó todo.

## Lo que sigue

Me gusta tener siempre un proyecto personal entre manos. El siguiente en la lista quizá sea una recompilación de
*Super Mario Odyssey* que se mantenga estable a 60 fps, aunque tengo la impresión de que ésa es mucho más difícil.

## Contacto

Dudas, errores y cualquier otra cosa, incluido un titular de derechos que pida retirar algo, por
[GitHub Issues](https://github.com/zWalt01/Country-Box/issues).
