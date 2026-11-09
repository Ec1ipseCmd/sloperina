# Working on sloperinaGZ

Read [`docs/FINDINGS.md`](docs/FINDINGS.md) first: it covers how the game works, every problem already solved and
the approaches that are known not to work. [`docs/BUILDING.md`](docs/BUILDING.md) covers the build and how the module loads.

## Rules that saved a lot of time

- **Don't guess, find the game's own way.** Before changing a value by hand, check how the game does it: the event
  scripts (`tools/evdump.py`) and the game's files (`tools/gamefiles.py`) show which node, parameters or pouch
  items it uses, and the module can call the game's own code. Most "it doesn't show / doesn't work" bugs were the
  game keeping a second copy of the state (Link's gear objects, the item wheel, UI screens) that only its own code
  updates.
- **Verify before asking someone to test.** Check the log (Lichii: `%APPDATA%\eden\log\eden_log.txt`, lines
  tagged `sloperinaGZ:`). Window capture can't see the emulator's picture, so check drawing by compiling the
  drawing code on the PC and rendering a panel to an image.
- **Never write save files.** Live memory only; settings go to the SD card (`sd:/sloperinaGZ/settings.ini`).
- **Watch out for the menu's warp in tests:** it leaves some state unset (e.g. the explored room), which once made
  working changes look broken.

## After changing code

1. `./docker-make dist`. It must build without warnings; the link fails if the module would import a symbol that
   isn't in `lib/imports.txt` (add one only after checking main or sdk exports it).
2. Copy `dist/lichii/load/0400486024DEC000/sloperinaGZ/exefs/*` into the emulator's
   `load/0400486024DEC000/sloperinaGZ/exefs/` (or install as in [`INSTALLATION.md`](INSTALLATION.md)) and restart the game.
3. Check the log for errors.

After changing the JSON data in `data/`, run `python3 tools/gendata.py` (writes `src/ootgz/data.c`). After changing menu
text, run `python3 tools/lang_template.py` (writes `lang/template.json`).

## Docs

- Add every verified finding to `docs/FINDINGS.md`: symptom, cause, fix, how it was found, and anything that was
  tried and didn't work (§6).
- `python3 tools/build_docs.py` regenerates the field / quest / tutorial reference in `docs/reference/`.
