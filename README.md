# ootmenu

A gz-style practice menu for **The Legend of Zelda: Ocarina of Time (Switch 2)** running in the **Lichii**
emulator on Windows. It's an overlay that reads and writes the game's live memory, plus a small code patch that
lets the game run its own inventory and UI code on request.

Supported version: program `0400486024DEC000`, update v131072 (main build `E0F3CCFA…`). Other versions have
different addresses and won't work.

**Start here if you want to work on it:** [`docs/FINDINGS.md`](docs/FINDINGS.md) documents how the game's data
works, the code patch, every problem solved so far (cause, fix, how it was found) and the approaches that don't
work. [`CONTRIBUTING.md`](CONTRIBUTING.md) has the build and test routine.

## Features

Dimmed entries in the menu are placeholders that aren't wired up yet.

- **link**: map, health, rupees, wallet, position. **equips** shows what Link wears. A levitate hotkey
  (`levitate_combo` in settings) floats Link upward like gz's.
- **inventory**: items, owned gear, ammo, capacities, magic, bottles, upgrades, quest items, songs, dungeon items,
  small keys. Ticking and unticking goes through the game's own code where the game needs it:
  - **Ocarinas** are given and removed the way the game does it: Link holds a playable ocarina, the D-pad appears,
    the HUD redraws.
  - **Upgrades** (wallet, bags, quiver, strength, scale, magic meter, double defense) hold the real pouch items, so
    the pause screen and story checks see them.
  - **Unticking** worn gear takes it off Link; unticking a button item clears it from the item buttons.
  - **Dungeon map / compass / boss key** update the pause map screen at once.
- **warps**: any map and spawn point.
- **cheats**: infinite energy, rupees, magic, ammo.
- **story / flags**: quest stages, tutorial guides, event flags (`flags.py` searches and sets them from the command
  line).
- **watches**: health, rupees, map, position, custom memory watches.

## Requirements

- Windows, Lichii with the game, run windowed or borderless fullscreen (not exclusive fullscreen).
- Python 3.11+ with `numpy` (`pip install -r requirements.txt`).
- For the research tools only: `capstone`, `keystone-engine`, `evfl`, `zstandard`.

No game files are included. The data files here (field names, hashes, warp lists) are derived from the game and
from community tools; you need your own copy of the game.

## Use

1. Double-click `run.bat`. A notice shows briefly in the top-left corner, then ootmenu waits in the background.
   Run `run.bat` again to stop it.
2. The first time (and after updates), the menu installs its code patch and shows "patch updated, please restart
   the game": restart the game in Lichii once.
3. Open the menu with **RB + right stick down** (the "R + C-down" equivalent), or **F3**.

| Button | Action |
|---|---|
| D-pad / arrow keys | Move; Left/Right change a value |
| LB / Enter | Select |
| `return` item / Backspace | Back one menu |
| RB + D-Left | Close the menu |

By default the emulator is paused (its F4 hotkey) while the menu is open; changes that need the game's own code
run when you close the menu. Settings save to `settings.json` (created on first change); button names are
`A B X Y LB RB LT RT START BACK DUP DDOWN DLEFT DRIGHT LSTICK RSTICK C-UP C-DOWN C-LEFT C-RIGHT`.

`python ootmenu.pyw --test` opens the menu as a normal window without the emulator. The log is
`%TEMP%\ootmenu.log`.

## Layout

- `ootmenu.pyw` — the overlay, controls and menu contents.
- `menu.py` — menu model. A feature becomes live once its item gets a handler.
- `game.py` — finds the game in memory; save fields, strings, Link, warps, equipment objects.
- `patch.py` — the code patch (bytes, install, request protocol) and the helpers that use it
  (`give_gear`, `unequip`, `clear_buttons`, `set_upgrade`, `refresh_screen`).
- `win32.py` — emulator window, overlay, keyboard/XInput, process memory.
- `flags.py` — search and set flags from the command line; `python flags.py backup` copies your saves.
- `gamedata.py`, `guestmem.py`, `scan.py` — research helpers (MurmurHash3, RAM block, value scanner).
- `*.json` — field names and types, story stages, warp points (see `docs/FINDINGS.md` §1).
- `tools/` — research tools: patch assembler (`hook_asm.py`), static analysis (`re64.py`), game files
  (`nupfs.py`, `byml.py`, `evdump.py`), snapshots (`snap.py`), GDB watchpoint tools (`dpadwatch.py`,
  `readwatch.py`, `magicwatch.py`).
- `docs/` — findings (`FINDINGS.md`) and a browsable field/quest/tutorial reference (`index.html`; rebuild the
  generated pages with `python docs/build.py`).

## Credits

- Link's health and the guest address mapping come from the oot26-tools Cheat Engine table
  (github.com/mrissaoussama/oot26-tools), which also provided the warp list.
- `.nup` layout from Hookshot's `nx2/nup.py`.
- Field titles from the onif save editor's demo save.
