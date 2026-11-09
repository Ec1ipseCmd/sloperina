# sloperinaGZ

A gz-style practice menu for **The Legend of Zelda: Ocarina of Time (Switch 2)**, built the way gz is: C code
the game loads and runs itself (an exefs module, `subsdk9`, plus a 67-byte IPS patch to main). It works from
Atmosphère's LayeredFS, an emulator's mod folder (Lichii / Eden: `load/`) or an offline patcher.

Supported version: program `0400486024DEC000`, update v131072 (main build `E0F3CCFA…`). Other versions have
different addresses and won't work.

- [`INSTALL.md`](INSTALL.md): installing it, languages, settings.
- [`BUILDING.md`](BUILDING.md): building it, how it loads, what's verified.
- [`docs/FINDINGS.md`](docs/FINDINGS.md): how the game's data works and every problem solved so far (cause, fix,
  how it was found) and the approaches that don't work. **Start here if you want to work on it.**
- [`CONTRIBUTING.md`](CONTRIBUTING.md): the build and test routine. [`CONTRIBUTORS.md`](CONTRIBUTORS.md) and
  [`THANKS.md`](THANKS.md): who made it and special thanks (both shown in the about menu, read at build time).

Open the menu with **R + right stick down**. **L** selects, the D-pad moves and changes values, **R + D-left**
goes back.

## Features

- **link**: health, heart containers, rupees, age and reload map; position, speed and map.
- **warps**: any map and spawn point, at once, with the game's own fade.
- **scene**: reload the map; the map's air walls drawn over the game (collision).
- **cheats**: infinite health, rupees, magic and ammo; 2x movement speed.
- **inventory**: items, equipment, collection (upgrades, strength, scale, magic), bottles, quest items, songs,
  dungeon items and small keys, all through the game's own code where it needs it.
- **equips**, **story** (quest stages, tutorials).
- **watches**: gz's watch list: built-in watches (position, speed, angles, camera, input, timer, lag...) and
  memory watches, with display types, labels (gz's keyboard), show/hide, reordering and the wrench to see the value
  in memory; the box can go in any corner or anywhere.
- **debug**: heap and gz's memory viewer (edit, add watches, follow pointers).
- **settings**: language, menu size and background, the input display (gz's row or a Switch 2 Pro Controller),
  levitate (L3), frame advance (L or R) with gz's pause icons.
- gz's log of what was done, bottom right.

## Layout

- `src/ootgz/`: the module (C). `data.c` is generated, by `tools/gendata.py`.
- `src/host/mkips.c`: makes the IPS patch. `lib/`: linker script and the list of symbols it may import.
- `Makefile`, `docker-make`: the build (devkitA64, or the devkitpro/devkita64 Docker image).
- `*.json`: the game's field names, hashes, story stages, warp and spawn points, air walls (inputs of
  `tools/gendata.py` and `docs/build.py`; see `docs/FINDINGS.md` §1).
- `tools/`: development tools in Python (not needed to play):
  - `gendata.py` regenerates `src/ootgz/data.c` from the JSON files; `lang_template.py` writes
    `lang/template.json` for translators.
  - Reading the game's files from a dump (set `OOTMENU_NUP` to the `.nup`): `nupfs.py`, `byml.py`,
    `gamefiles.py`, `airwalls.py` (→ `airwalls.json`), `spawnstarters.py` (→ `spawn_starters.json`), `evdump.py`
    (event flows).
  - `re64.py`: static analysis of a dump of main (cross-references, disassembly with field names).
  - `requirements.txt` lists what they need.
- `lang/`: the translation template.
- `docs/`: findings and a browsable field / quest / tutorial reference (`index.html`; rebuild with
  `python docs/build.py`).

The first version was a Python overlay for Lichii (ootmenu) that read and wrote the game's memory from outside;
FINDINGS.md still names its files where they're how something was found. It's in this repository's history
(commit `6d8cdba` and before) and archived next to it (`ootmenu-python-overlay-2026-11-09.tar.gz`).

No game files are included. The data files here are derived from the game and from community tools; you need
your own copy of the game.

## Credits

- Based on the work of OoT N64's Practice ROM, gz, by glankk. Font: Press Start 2P (codeman38, OFL).
- Link's health and the guest address mapping come from the oot26-tools Cheat Engine table
  (github.com/mrissaoussama/oot26-tools), which also provided the warp list.
- `.nup` layout from Hookshot's `nx2/nup.py`.
- Field titles from the onif save editor's demo save.
