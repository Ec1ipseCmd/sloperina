# Installing sloperinaGZ

Supported: Ocarina of Time (Switch 2), program `0400486024DEC000`, update v131072 (main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`). The patch is keyed to that build; on another
version it is simply not applied, and the module then does nothing.

Build first (`make dist`, see [BUILDING.md](BUILDING.md)). `dist/` holds three layouts of the
same two files: `subsdk9` (the code) and `<build id>.ips` (hooks it in). Both are needed.

## Lichii / Eden (and other yuzu-family emulators)

Copy `dist/lichii/load/` into the emulator's data folder (Lichii: `%APPDATA%\eden\load\`), so
you get `load\0400486024DEC000\sloperinaGZ\exefs\{subsdk9, E0F3….ips}`.

Remove or disable `load\0400486024DEC000\ootmenu` first: the old Python menu's patch uses the
same hook site, so only one of the two can be active. Restart the game after installing.

## Console with LayeredFS (Atmosphère layout)

Copy `dist/atmosphere/` to the root of the SD card:

```
atmosphere/contents/0400486024DEC000/exefs/subsdk9
atmosphere/exefs_patches/sloperinaGZ/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips
```

This is the layout Atmosphère uses on Switch 1: exefs files added through LayeredFS, IPS
patches applied to modules by build id. sloperinaGZ needs nothing else (no `main.npdm` change, no
extra system calls). Whether a Switch 2 custom firmware exists and follows this layout is
outside this project. The files are what any loader needs, in whatever folders it reads.

## Offline patching

`dist/exefs/v131072/` has the two files for a patch applier that rebuilds the game's exefs:
add `subsdk9` as a new exefs file, and apply the IPS32 to `main`. Offsets in the IPS include
the 0x100-byte NSO header, the usual convention for exefs patches.

## Using it

- **R + right stick down** opens the menu (gz's R + C-down) and closes it again; **R + D-left**
  goes back one level (and closes the menu at the top level).
- D-pad up/down to move, left/right to change a value, **L** to select (as in gz); pick
  `return` to go back a menu.
- Watches show in the bottom-left corner while the menu is closed (watches menu: which ones, and
  where: any corner, top-centre or bottom-centre);
  **R + D-right** shows/hides them, **R + A + D-left** starts/stops the timer,
  **R + B + D-left** resets it, **R + B + D-right** resets the lag counter (as in gz).
- **L3** starts levitating (Link rises), **L3** again drops him (fall damage on landing is given
  back); hold **R3** to keep the height; the **left stick** moves him (walls stop him unless
  settings > levitate through walls is on). The first press finds Link's position, which takes a
  moment. While levitate is on the game doesn't see L3 (nor R3 while Link floats); settings has
  the switch and the rise and move speeds.
- cheats > **2x movement speed** doubles how far Link moves on the stick (the first time, it finds his
  position the same way, with a small hop).
- Warps: pick a place and it starts at once. If the game won't start it, a notice says so and
  the next exit or loading zone you walk through takes you there instead.
- The menu is drawn under the HUD's D-pad / ocarina icon (long menus scroll). settings >
  background opacity makes the panels see-through (below 100% it costs some speed, and what's
  behind lags by two frames). The game ignores the controller while the menu is open. If drawing can't start, the log says why
  (`on-screen drawing off: ...`) and the menu is printed to the log instead.
- Settings (cheats, watches, background opacity, arrival cutscenes) are saved to the SD card as
  `/sloperinaGZ/settings.ini` and loaded at boot (Lichii: `%APPDATA%\eden\sdmc\sloperinaGZ\settings.ini`). It's
  plain `key=value` text. On a console the game needs SD card access for this; without it a notice
  says settings won't be kept and everything else works. The game's save data is never touched.

## Settings from before the rename

sloperinaGZ was called ootgz. Its settings and language files now live in `sd:/sloperinaGZ`; if
that folder has no `settings.ini` yet, the old `sd:/ootgz/settings.ini` is read (and saved again in
the new folder), and language files are also looked for in `sd:/ootgz/lang`. Remove an old
`load/0400486024DEC000/ootgz` (or `exefs_patches/ootgz`) folder: two copies of the module conflict.

## Languages

The menu is in English. To translate it, copy `dist/sd/sloperinaGZ/lang/template.json` (made by
`tools/lang_template.py`) to the SD card as `sd:/sloperinaGZ/lang/<code>.json`, where `<code>` is one of
`es fr de it pt nl pl sv da no fi cs hu tr ro ca id custom`. On Lichii the SD card is
`%APPDATA%\eden\sdmc`. In the file:

- set `"language"` to the language's own name (shown in **settings → language**);
- fill in the translations; anything left `""` stays English;
- keep every `%s`, `%d`, `%.2f` and so on of a string, in the same order (a translation whose
  conversions don't match is ignored, so a mistake can't crash the game).

The font has plain ASCII only: accented letters are shown without their accents, and other scripts
(Japanese, Cyrillic...) can't be shown yet. Language files are looked for when the game starts.
