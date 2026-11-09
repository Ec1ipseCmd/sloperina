# sloperinaGZ

A gz-style practice menu for **The Legend of Zelda: Ocarina of Time (Switch 2)**, built the way gz is: C code
the game loads and runs itself (an exefs module, `subsdk9`, plus a 143-byte IPS patch to main). It works from
Atmosphère's LayeredFS, an emulator's mod folder (Lichii / Eden: `load/`) or an offline patcher.

Supported version: program `0400486024DEC000`, update v131072 (main build `E0F3CCFA…`). Other versions have
different addresses and won't work.

Source: [github.com/PracticeROM/sloperina](https://github.com/PracticeROM/sloperina). Website:
[practicerom.com/oothd](https://practicerom.com/oothd).

- [`docs/USAGE.md`](docs/USAGE.md): how to use it (quick start). [`docs/MANUAL.md`](docs/MANUAL.md): every menu and option.
- [`INSTALLATION.md`](INSTALLATION.md): installing it (with the installer or by hand), updating, removing, languages.
- [`docs/BUILDING.md`](docs/BUILDING.md): building it, how it loads, what's verified.
- [`docs/FINDINGS.md`](docs/FINDINGS.md): how the game's data works and every problem solved so far (cause, fix,
  how it was found) and the approaches that don't work. **Start here if you want to work on it.**
- [`docs/COLLISION.md`](docs/COLLISION.md): the bounded collision view, its limits, and the decoded
  collision file format.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): the build and test routine. [`CONTRIBUTORS.md`](CONTRIBUTORS.md) and
  [`THANKS.md`](THANKS.md): who made it and special thanks.

Open the menu with **R + right stick down**. **L** selects, the D-pad moves and changes values, **R + D-left**
goes back.

## Features

- **link**: health, heart containers, rupees, age and reload map; position, speed and map.
- **warps**: any map and spawn point, at once, with the game's own fade.
- **scene**: reload the map; separate nearby floor, wall, ceiling, and static-trigger filters; invisible barriers
  and supported player void-sensor bounds, with optional void-out suppression.
- **cheats**: infinite health, rupees, magic and ammo; 2x movement speed.
- **inventory**: items, equipment, collection (upgrades, strength, scale, magic), bottles, quest items, songs,
  dungeon items and small keys, all through the game's own code where it needs it.
- **equips**, **story** (quest stages, tutorials).
- **watches**: gz's watch list: built-in watches (position, speed, angles, camera, input, timer, lag...) and
  memory watches, with display types, labels (gz's keyboard), show/hide, reordering and the wrench to see the value
  in memory; the box can go in any corner or anywhere. **Enemy Health** shows current/max above nearby enemies.
- **debug**: heap and gz's memory viewer (edit, add watches, follow pointers).
- **settings**: language, menu size and background, the input display (gz's row or a Switch 2 Pro Controller),
  levitate (L3 toggle, held X/B up/down, release to hover), frame advance (L or R) with gz's pause icons.
- gz's log of what was done, bottom right.

## Layout

- `src/ootgz/`: the module (C). `data.c` is generated, by `tools/gendata.py`.
- `src/host/mkips.c`: makes the IPS patch. `lib/`: the linker script and the list of symbols the module may
  import.
- `Makefile`, `docker-make`: the build (devkitA64, or the devkitpro/devkita64 Docker image). `VERSION`: the
  release number shown in the about menu; `CONTRIBUTORS.md`, `THANKS.md`: the credits.
- `data/`: the game's field names, hashes and types, story stages, warp and spawn points, air walls (derived from
  the game; inputs of `tools/gendata.py` and `tools/build_docs.py`; see `docs/FINDINGS.md` §1).
- `lang/`: the translation template (`tools/lang_template.py`).
- `docs/`: [`FINDINGS.md`](docs/FINDINGS.md), [`BUILDING.md`](docs/BUILDING.md), [`COLLISION.md`](docs/COLLISION.md),
  [`INSTALLATION.md`](INSTALLATION.md), and `reference/`, a browsable field / quest / tutorial reference
  (`reference/index.html`; rebuilt by `tools/build_docs.py`).
- `tools/`: development tools in Python (not needed to play; `tools/requirements.txt` lists what they need):
  - `gendata.py` regenerates `src/ootgz/data.c` from `data/`; `lang_template.py` writes `lang/template.json`
    for translators; `build_docs.py` regenerates `docs/reference/`.
  - Reading the game's files from a dump (set `OOTMENU_NUP` to the `.nup`): `nupfs.py`, `byml.py`,
    `gamefiles.py`, `airwalls.py` (→ `data/airwalls.json`), `spawnstarters.py` (→ `data/spawn_starters.json`),
    `evdump.py` (event flows), `phive.py` (collision; see `docs/COLLISION.md`).
  - `re64.py`: static analysis of a dump of main (cross-references, disassembly with field names).
- `bin/`, `obj/`, `dist/`: build output (not in git).

The first version was a Python overlay for Lichii (ootmenu) that read and wrote the game's memory from outside;
FINDINGS.md still names its files where they're how something was found. It's in this repository's history
(commit `6d8cdba` and before) and archived next to it (`ootmenu-python-overlay-2026-11-09.tar.gz`).

No game files are included. The data files here are derived from the game and from community tools; you need
your own copy of the game.

## Licence

sloperinaGZ is free software under the GNU General Public License, version 3 ([LICENSE](LICENSE)), like gz.

## Credits

- Based on the work of OoT N64's Practice ROM, gz, by glankk. Font: Press Start 2P (codeman38, OFL).
- Link's health and the guest address mapping come from the oot26-tools Cheat Engine table
  (github.com/mrissaoussama/oot26-tools), which also provided the warp list.
- `.nup` layout from Hookshot's `nx2/nup.py`.
- Field titles from the onif save editor's demo save.
