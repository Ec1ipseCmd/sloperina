# Working on ootmenu

Read [`docs/FINDINGS.md`](docs/FINDINGS.md) first: it covers how the game works, every problem already solved and
the approaches that are known not to work.

## Rules that saved a lot of time

- **Don't guess, find the game's own way.** Before changing a value by hand, check how the game does it: the event
  scripts (`tools/evdump.py`) show which node and parameters it uses, and the patch can run the game's own code
  (`patch.call_function`, item requests). Most "it doesn't show / doesn't work" bugs here were the game keeping a
  second copy of the state (Link's gear objects, the item wheel, UI screens) that only its own code updates.
- **When the cause isn't obvious, catch it.** A GDB watchpoint on the bytes that change (`tools/dpadwatch.py`,
  `tools/readwatch.py`) finds the responsible code in minutes; static tracing by hand can take hours.
- **Verify before asking someone to test.** Read the fields back, take a screenshot (see FINDINGS §7), check
  `%TEMP%\ootmenu.log`.
- **Never write save files.** Live memory only. `python flags.py backup` copies the save folder to
  `save_backups/<time>` (ignored by git); to restore, copy a backup back while the game is closed.
- **Watch out for the menu's warp in tests:** it leaves some state unset (e.g. the explored room), which once made
  working changes look broken.

## After changing menu code

1. Stop the running menu (close `pythonw`, or run `run.bat` again).
2. Run `python ootmenu.pyw` from this folder for ~10 seconds and check `%TEMP%\ootmenu.log` for `error:` lines.
   A syntax check isn't enough: some errors only show once the game is found.
3. Start it normally (`run.bat`).

## After changing the code patch

See FINDINGS §3 "Changing the patch": edit `tools/hook_asm.py` / `patch.ITEMS`, bump `patch.VERSION`, paste the
assembled bytes into `patch.py` (check `matches patch.py: True`), restart the menu, then restart the game. The
patch only takes effect when the game starts.

## Docs

- Add every verified finding to `docs/FINDINGS.md`: symptom, cause, fix, how it was found, and anything that was
  tried and didn't work (§6).
- `python docs/build.py` regenerates the field / quest / tutorial reference pages in `docs/`.
