# About Country-Box

**[Español](docs/ABOUT.es.md)**

Hi, I'm **Walter**. I study data science, and Country-Box started as a question: *how hard is it, really, to make one
of these recompilation projects?*

I couldn't find any recompilation of these two games, and the ones I did find for other games were rarely built with
the Steam Deck in mind: easy to set up, and fast once they run. So I tried to make the one I wanted to use: you bring
your own copy, press one button, and play it natively on Linux or on the Deck at a steady 60 fps.

It turned out to be very hard with some models — partly because I didn't know them well — and almost scarily easy
with others (Claude Opus 5.5).

## What I did

- Took the recompilation core of [Metroid Prime 4 Recomp](https://github.com/jaigner-hub/metroid-prime-4-recomp)
  (Jeff Aigner, MIT) — itself built on fe3h-pc-recomp, totk-pc-recomp, LAS Recomp and others — and ported its runtime
  from Windows to Linux and the Steam Deck.
- Made *Donkey Kong Country Returns HD* (a Unity game) and *Tropical Freeze* (Retro's own engine) boot, play, and hold
  60 fps — Returns HD across all 81 of its levels, on PC and on the Steam Deck.
- Built the shader preparation that runs before you play, so the game doesn't stutter compiling shaders mid-level
  (complete for Returns HD; Tropical Freeze still has a known gap).
- Wrote the launcher: it checks your copy, builds the game on your own machine, keeps your saves safe across updates,
  and updates itself.
- Measured everything. Every number in this repository has a date and a source in [docs/TESTING.md](docs/TESTING.md);
  what wasn't measured is said plainly.

## How it was made

I built Country-Box by directing AI coding agents (Anthropic's Claude models, and Z.ai's GLM). I set the goals, made
the decisions, played the games and judged the results; the agents wrote and tested code under that direction, and
reviewed each other's work. Every claim was checked against a real run before it was written down, and every line
that ships was reviewed before publishing, including a line-by-line pass for personal data and anything that belongs
to the games' owners.

I'm saying this openly because I think it's part of the answer to the question this started with.

## What's next

I like to always have a side project in hand. Next on my list is maybe a *Super Mario Odyssey* recompilation that
holds a stable 60 fps — though I have a feeling that one is much harder.

## Contact

Questions, bugs, and anything else — including a rights holder asking for something to be removed — go through
[GitHub Issues](https://github.com/zWalt01/Country-Box/issues).
