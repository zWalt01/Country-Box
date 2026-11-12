# Installing Country-Box with an AI agent

Copy the prompt below into your coding agent (any agent that can run shell commands on your Linux machine or
Steam Deck in Desktop Mode). It drives the launcher's command line, which answers in JSON, so the agent can follow
the installation step by step and fix what fails.

---

## Prompt

You are installing **Country-Box**, a native Linux port of *Donkey Kong Country Returns HD* and *Donkey Kong
Country: Tropical Freeze*, for the user on this machine. Follow these steps in order. Run every command yourself,
read its JSON output, and stop to ask the user only where a step says so.

**Rules — never break them**

- Country-Box ships no game. The user must already have **their own extracted copy**: a folder with `exefs/` and
  `romfs/` inside. Never download a game, never search for one, never ask for or handle keys, never decrypt or
  extract anything. If the user has no extracted copy, stop and tell them that is required.
- Do not use `sudo` without telling the user first and getting a yes. Do not touch files outside the launcher's
  home (`~/.local/share/country-box`, or the folder passed with `--home`) and the folder the user downloaded.
- Report what failed in plain words, with the launcher's `headline`, `hint` and `log` fields.

**1. Get the launcher.** Download the latest release from <https://github.com/zWalt01/Country-Box/releases> and
unpack it. The folder contains `country-box-launcher` and `prep/`. Work from that folder:

```sh
cd <unpacked folder>
./country-box-launcher --status
```

`--status` prints every game with a `state`: `not_installed`, `copy_missing`, `needs_check`, `needs_build`,
`needs_shaders`, `ready` or `update_available`, plus a `detail` sentence.

**2. Check the tools.** Building needs `clang`, `lld`, `ninja`, `pkg-config`, `python3` (with its `venv` module), and
the development packages of `sdl3`, `vulkan`, `opus`, `vpx` and `zlib`. If something is missing, `--prepare` (step
4) stops with `kind: missing_tools` and its `hint` holds the exact package-manager command for this distribution: show
it to the user and run it only after they say yes (it needs `sudo`). The Python modules are not your job: on the
first Prepare the launcher makes its own environment in `~/.local/share/country-box/python` and installs them
(needs the network, about 40 MB).

**3. Add the user's copy.** Ask the user for the path of their extracted folder (the one containing `exefs/` and
`romfs/`). Then, with `GAME` = `dkcrhd` (Returns HD) or `dkctf` (Tropical Freeze):

```sh
./country-box-launcher --add GAME /path/to/their/copy
```

`"ok": true` means every file matched the supported version. Otherwise read `explain` and `details`: a different
version (usually an update applied), missing or damaged files, or the other game's copy. Tell the user; do not try
to modify their copy.

**4. Build it for this machine.**

```sh
./country-box-launcher --prepare GAME
```

This converts the game's code from their copy into C, compiles it and prepares the shaders. It prints progress
lines and ends with one JSON line. Expect minutes: on an 8-core PC, Tropical Freeze took about 3 minutes and Returns
HD about 20. It can be interrupted and run again: finished work is kept. On failure, `kind` says what went wrong:

| `kind` | What to do |
|---|---|
| `missing_tools` | install what `explain` lists (step 2), then run `--prepare` again |
| `disk` | free the space `explain` asks for, or use `--home` on a bigger drive |
| `copy`, `wrong_copy`, `wrong_game`, `not_found` | the copy changed or is not the right one: go back to step 3 |
| `lift`, `compile`, `shaders` | send the user the `log` path; this is a bug to report (step 6) |
| `payload` | the download is incomplete: download the release again |
| `cancelled` | it was stopped; run `--prepare` again |

A Steam Deck has no compiler. Build on a PC instead, copy the game's build folder
(`~/.local/share/country-box/games/GAME`) to the Deck, and run
`./country-box-launcher --import-build GAME /path/to/that/folder` there.

**5. Play.** Check that `--status` says `ready`, then start the game (or tell the user they can open
`country-box-launcher` without arguments for the window with controller support):

```sh
./country-box-launcher --play GAME
```

Updates: `./country-box-launcher --check-update` and `--update`. An update never touches saves or settings; after
it, run `--prepare GAME` again.

**6. If something breaks**, collect the `log` path from the failing step and the output of `--status`, and help the
user open an issue at <https://github.com/zWalt01/Country-Box/issues>. Never paste anything from the game's files.
