# ootmenu findings

Everything verified while building ootmenu, written so the next person doesn't have to rediscover it. Each solved
problem lists the symptom, the cause, the fix, how it was found, and the approaches that **don't** work.

Applies to **Ocarina of Time (Switch 2), program `0400486024DEC000`, update v131072, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`**, running in **Lichii** (an Eden/yuzu fork, Windows). Other versions
have different addresses.

Conventions: `main+0x…` is an offset into the game's main module. Field hashes are MurmurHash3_32 (seed 0) of the
field name (`gamedata.murmur3`). "The patch" is ootmenu's code patch (`patch.py`, source in `tools/hook_asm.py`).

## Contents

1. [Memory and save data](#1-memory-and-save-data)
2. [Game files](#2-game-files)
3. [The code patch](#3-the-code-patch)
4. [Game functions used](#4-game-functions-used)
5. [Solved problems](#5-solved-problems)
6. [Things that do not work](#6-things-that-do-not-work)
7. [How to investigate](#7-how-to-investigate)
8. [Open problems](#8-open-problems)

---

## 1. Memory and save data

### Finding the game
- Guest (Switch) addresses map linearly into Lichii's process: `host = guestbase + guest`. `game.Game` finds
  `guestbase` and the main module base (`game.M`, changes every launch) and checks them against a known pointer
  (`CHECK_OFF`/`CHECK_VAL_OFF` in `game.py`).
- Main's code ends at `main+0x3c461d0`; rodata starts at `main+0x3c47000`. The gap is mapped executable and
  zero-filled (used for the patch, see §3).

### GameData (the live save fields)
- All saved state lives in GameData fields, keyed by the hash of the field name. The full list (~15,000 fields with
  types and defaults) ships in the game: `romfs/GameData/GameDataList.Product.110.byml.zs`. Exported to
  `gamedata_fields.json`. About 4,600 names were recovered by hashing every string in the game's scripts and data;
  ~3,600 more fields have readable titles from the onif save editor (`all_fields.json`, mapped by diffing exports).
- GameData keeps a **type list** of `(type vtable, count, objects, hash table)` entries, one per field type. The
  hash table maps field hash → object index. Objects of one type are a fixed size apart:
  - Int / Float: 0x28 bytes, value at `+0x1C`, **copy at `+0x20`** (write both).
  - Enum: 0x38 bytes, value (MurmurHash3 of the choice name) at `+0x1C`, copy at `+0x20`.
  - Bool: 0x18 bytes, value byte at `+0x16`, copy at `+0x17`.
  - String64: 0x108 apart (`game.STR_STRIDE`); ootmenu reaches a few by index (`game.STR_IDX`): `sword` 0x17 =
    `PlayerStatus.EquipWeapon`, `shield` 0x16 = `EquipShield`, `tunic` 0x10, `boots` 0x12, `ocarina` 0x15 =
    `EquipOcarina`, `map` 0x1F = current map.
- Live access: `game.Game` — `get_field/set_field` (Int by hash), `get_bool/set_bool`, `get_typed/set_typed`
  (`"enum"`, `"float"`), `get_str/set_str` (String64 by `STR_IDX` key).

### Field types that matter
- **Struct** fields group others (`PlayerStatus`, `ActionGuide_IsEnd`, `DungeonInfo.<Dungeon>`); members are
  separate fields named `Struct.Member`.
- **BoolExp** fields are formulas the game evaluates (e.g. "quest at or past stage X"); they can't be written.
- **Enum** values are hashes of choice names; the choices are in `gamedata_fields.json` (`"v"`).

### Save files
- `%APPDATA%\eden\nand\user\save\0000000000000000\<user id>\0400486024DEC000\slot_NN\progress.sav`: from offset
  0x20, `(hash u32, value u32)` pairs; for String64 fields the value is the file offset of a 64-byte text slot. No
  checksum. `caption.sav` holds the location and the save date (also hash/value pairs).
- Saving writes back what Link actually holds (e.g. `PlayerStatus.Equip*`).

### Useful name patterns
- `PouchData.<item>.StockNum` — count owned. `IsGet.<item>` — "ever acquired" flag. An item needs both to show on
  the item screen. The L/R quick menus only open with two or more items acquired.
- `PlayerStatus.EquipWeapon/EquipShield/EquipArmor/EquipBoot/EquipOcarina` (String64): what Link wears. **These
  follow pause-menu equips live** (not only on save).
- `HavePlayedEvent.<event>`, `HaveTalked_Special.<npc>_<event>`: event history. `Step_<quest>`: quest stage (Enum;
  setting the stage works after a loading zone; setting "played event" flags alone does nothing).
- `ActionGuide_IsEnd.<guide>`: tutorial screens seen. **The D-pad only exists once `ActionGuide_IsEnd.Navi` is set**
  (normally on entering the Deku Tree).
- `DungeonInfo.<Dungeon>.IsGetMap / IsGetCompass / IsGetBossKey` (Bool). Dungeon names: `InsideTheDekuTree`,
  `DodongoCavern`, `InsideJabuJabuBelly`, `ForestTemple`, `FireTemple`, `WaterTemple`, `ShadowTemple`,
  `SpiritTemple`, `BottomOfTheWell`, `IceCavern`, `GerudoTrainingGround`, `InsideGanonCastle` (same as the map name).
- **Item buttons**: four Enum fields with unrecovered names — `0x6bee602d`, `0x76f85e6d`, `0xa0029837`,
  `0xbc32ce24`. Their choices are the assignable items (`Empty`, `DekuStick`, `SlingShot`, `Bow`, `HeavyBoot`,
  bottles, masks, …). Found by searching Enum fields whose choice list holds `SlingShot`; confirmed live.
- `SimpleIntForMessage`, `…2`, `…3`, `SimpleHourForMessage`, `SimpleHourForMessageAs24Hour`, `SimpleMinForMessage`:
  message-formatting scratch Ints, never saved. **The game writes them** (e.g. its save-date text at
  `main+0x33cc94c` puts year/month/day/hour/minute there), see §5.3.

### Magic
- The real current magic lives in the magic meter object (holds the hashes of `Current magic` / `Magic capacity` at
  +0x38/+0x3C, value at +0x44). Every frame the game writes `clamp(value, 0, capacity)` into the save field, so
  writing only the field is undone (`game.set_magic` writes both). Capacity 0 = no meter; 240 normal, 480 double.

## 2. Game files

- The game ships as a `.nup` package (layout from Hookshot's `nx2/nup.py`); `tools/nupfs.py` lists/extracts it.
- Most romfs files are zstd with dictionaries from `Pack/ZsDic.pack.zs`; BYML files read with `tools/byml.py`.
- Event scripts `Event/EventFlow/*.bfevfl` (format v0.4) decode with the `evfl` Python package;
  `tools/evdump.py` lists what each flow sets and checks. **This is the fastest way to learn how the game itself
  does something** (e.g. which node and parameters a cutscene uses to give an item).
- `tools/re64.py` does static analysis on a dump of main (`main.bin` = raw image from main's base, `main.base` = its
  load address in hex): string/function cross-references (`xref`), disassembly with field names (`dis`, `func`).

## 3. The code patch

Lichii applies IPS/IPS32 patches from
`%APPDATA%\eden\load\0400486024DEC000\ootmenu\exefs\E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips` **at game start
only** (it translates and caches code; a running game never sees a new patch). Offsets in the file are main
offsets + 0x100 (module header). The menu installs/updates the file on start (`patch.install`) and checks the
running game's memory to see whether the current version is active (`patch.active`); it shows "patch updated,
please restart the game" until it is.

### Layout
- Code cave: `main+0x3c46400`..`0x3c47000` (Lichii's own fixes use `0x3c46200`–`0x3c46278`). Marker
  `OOTMENU\0` + version at `0x3c46400`, hook code at `0x3c46440`, item names (32 bytes each) after it.
- Hook site: every frame `main+0x3b500a4` locks GameData, applies its command queue (`main+0xb99980`) and tail-calls
  the unlock at `main+0x3b500dc`. The patch redirects that tail call to the hook, which unlocks, then handles one
  request. So requests run **on the game's thread, once per frame, while the game is running** (not while paused by
  the emulator).

### Request protocol (`patch.py`)
The menu writes a request into the mailbox Int `SimpleIntForMessage3` (`0xE670579A`); the hook reads it with the
game's GetInt, and if it's tagged, clears it with SetInt and acts:

```
value = 0x0CA7 << 16 | remove << 9 | IsEquipDirect << 8 | item      item = 1..len(ITEMS)
value = 0x0CA7 << 16 | 0x7F                                         call request
```

- **Give** (`patch.request_item`): AddPouch exactly as the `OneShotAddOrRemovePouchContent` event node does:
  `main+0x110d570(*[main+0x4c7e0d0], &name, 0, 1, 0, 0, 0, IsEquipDirect, [sp]=0)`, then its notify
  `main+0x110d0b0(*[main+0x4e6b718], &name, ok, 0)`. `name` is `{char* top; s32 size = 0x80; char buf[]}`.
- **Remove** (`remove=True`): `main+0x33a4fa0(pouch manager, &name, 1, 0, 0, 0)`, what the same node does for a
  negative count (at `main+0x28b5140`).
- **Call** (`patch.call_function(game, fn, x0, x1)`): the game calls `main+fn(x0, x1)` once. Function and arguments
  are passed in the other message scratch Ints (written immediately before the request).
- `patch.ITEMS` is the name table baked into the patch; **its order is the item numbering**, so changing it means a
  new patch version and a game restart.
- After each item request the hook also sets the `SeqOneShotResetPlayerStatus` flags (harmless, but they don't
  redraw anything, see §6).

### Changing the patch
1. Edit `tools/hook_asm.py` (or `patch.ITEMS`), bump `patch.VERSION`.
2. `python -B tools/hook_asm.py` assembles it (needs `keystone-engine`, `capstone`) and prints the bytes; put them in
   `patch.HOOK_BYTES` and check it prints `matches patch.py: True`. Use `-B` and delete `__pycache__/patch*.pyc`
   between steps: a stale cached `patch` module once produced bytes for the old item table.
3. Restart the menu (installs the file), then restart the game.

## 4. Game functions used

| Address | What | Signature / notes |
|---|---|---|
| `main+0x163d20` | GameData GetInt | `(mgr, &out, hash)`; `mgr = *[main+0x4c7e180]` |
| `main+0x163b70` | GameData SetInt | `(mgr, value, hash)` |
| `main+0x110d570` | AddPouch (give) | see §3; negative counts do nothing |
| `main+0x33a4fa0` | Remove from pouch | `(pouch mgr, &name, count, 0, 0, 0)`; also re-applies Link's gear via `0xf201b0` |
| `main+0x2df6c80` | Create one equipment slot on Link | `(equipment component, slot)`; reads `PlayerStatus.Equip*`; slot `0xd` = ocarina. Doesn't take gear off |
| `main+0xf201b0` | Re-apply all of Link's gear | `(owner, 0)`; owner holds the component at `+0x470`; takes off empty slots |
| `main+0xa77ff0` | HUD screen method 98 | `(screen)`; redraws the HUD, turns the D-pad on |
| `main+0x33e12c0` | Map screen method 98 | `(screen)`; re-reads the dungeon flags |
| `main+0x837240` | UI manager: screen by hash | `(ui mgr, hash)`; HUD hash `0x5c7f1c3c` |

Objects:
- **Link's equipment component**: vtable `main+0x4b49698`, `+0x18` = Link's actor (`game.equip_component`; several
  actors have one, match the owner).
- **Its owner**: vtable `main+0x4bdfe68`, component at `+0x470` (`game.equip_owner`).
- **UI manager**: `*[main+0x4c7e098]`; screen count at `+8`, screen array at `+0x10`. **Every UI screen's virtual
  method 98 (`vtable+0x310`) is its "refresh contents"** — what opening it or unpausing runs.
  `patch.refresh_screen(game, vtable)` calls it. HUD (PlayerStatus screen) = vtable `main+0x4beb710`, holds the
  D-pad widget (vtable `main+0x4be1068`) at `+0x330`; pause map screen = vtable `main+0x4be8300`.

## 5. Solved problems

### 5.1 Ocarina shows on the D-pad but doesn't play
- **Symptom:** after giving the ocarina (pouch count, `IsGet`, `EquipOcarina`), the D-pad shows it but pressing the
  button does nothing. Area reloads, the ocarina tutorial flag, `IsEquipDirect` either way, and matching every
  tutorial flag to a working save made no difference.
- **Cause:** Link's usable gear is a separate object. Pouch data and `PlayerStatus.Equip*` are only data; the game
  creates the object with `main+0x2df6c80(equipment component, slot)`, which reads the slot's `PlayerStatus.Equip*`
  name (slot `0xd` = `EquipOcarina`). The full set is applied in slots 0, 6–10, 0xd–0x10 by `main+0xf201bc`.
- **Fix:** set `EquipOcarina`, give through the patch (Saria's way, `IsEquipDirect = 0`), then call
  `main+0x2df6c80(component, 0xd)` through the call request (`patch.give_gear`). Link then wears it at his waist.
- **Found by:** the string `EquipOcarina` is referenced once, in `main+0x2df6c80`; its callers apply every equip
  slot in turn.

### 5.2 D-pad / HUD doesn't redraw until a pause
- **Symptom:** after a give, the D-pad (and the ocarina on it) only appears after pausing.
- **Cause:** pausing/unpausing calls the HUD screen's virtual method 98 (`main+0xa77ff0`, via `main+0xd212c0`),
  which rebuilds the HUD and turns the D-pad on.
- **Fix:** call `main+0xa77ff0(HUD screen)` after the give (`patch.give_gear`).
- **Found by:** snapshot of the D-pad widget before/after a pause (bytes `+0x28` 0→2, `+0x2a` 0→1, `+0x189` 1→0),
  then GDB write watchpoints on them (`tools/dpadwatch.py`) caught the call chain from the pause.

### 5.3 Requests misfire / vanish
- **Symptom:** requests sometimes didn't run; the game sometimes "gave" an item by itself.
- **Cause:** the mailbox `SimpleIntForMessage3` is also written by the game's save-date text (day of month).
- **Fix:** tag requests (`0x0CA7 << 16`); the hook ignores and doesn't clear anything else (patch v6).

### 5.4 Unticking the ocarina leaves it with Link
- **Cause / fix:** AddPouch with a negative count does nothing; the game's node uses `main+0x33a4fa0` for removals.
  The single-slot call doesn't take gear off; the full re-apply `main+0xf201b0(owner, 0)` does once the slot is
  empty. So: empty `EquipOcarina`, remove (patch v8), re-apply, redraw the HUD, clear `IsGet`.
- **Found by:** event scripts with `Num=-1` (`tools/evdump.py`), then the node's code (`main+0x28b4a70`).

### 5.5 Unticking worn gear leaves Link wearing it
- **Fix:** if `PlayerStatus.Equip<slot>` is that item, empty it and call `main+0xf201b0(owner, 0)` + HUD redraw
  (`patch.unequip`). Gear itself stays plain data: the player equips it in the pause menu, which runs the game's
  own equip code.

### 5.6 Unticking an item leaves it on an item button
- **Fix:** set any of the four button Enums holding it to `Empty`, then redraw the HUD (`patch.clear_buttons`).
- **Gotcha:** the in-game item wheel keeps its own copy of the buttons and writes it back when it closes, so a clear
  done while it's open is undone. `clear_buttons` keeps watching for 2 minutes while the item stays unticked.

### 5.7 Upgrades (wallet, bags, quiver, strength, scale, magic, double defense) don't take
- **Symptom:** the capacity/depth number changes but the pause screen still shows the old item (e.g. Kokiri wallet
  with a 500 capacity).
- **Cause:** the game's screens and story checks use pouch items, not the numbers (e.g. the Lost Woods scrub runs
  `EventQueryIsExistInPouch(Item_Wallet_Medium)`).
- **Fix:** hold the matching item through the game's add/remove code (`patch.UPGRADES`, `patch.set_upgrade`) and still
  write the number. Item names: `Item_StickBag_*`, `Item_NutBag_*`, `Item_BulletBag_*`, `Item_BombBag_*`,
  `Item_Quiver_*`, `Item_Wallet_*` (Small/Medium/Large), `Item_Scale_Silver/Gold`, `Item_FairyBless_MP/MPDouble/
  DefenseDouble`, `Armor_Child_Gauntlet_Goron`, `Armor_Adult_Gauntlet_Silver/Gold`. Wallet verified in game.

### 5.8 Dungeon map / compass don't show
- **Symptom:** the map screen shows "???" after ticking the map.
- **Cause 1:** entering a dungeon through the menu's warp leaves no explored room registered, so the map screen shows
  "???" whatever you own. Entering through the door works. (Without the map the screen shows explored rooms; with
  it, the whole layout.)
- **Cause 2:** the map screen reads the flags when it opens. Ticking while it's open needs a refresh.
- **Fix:** the flags `DungeonInfo.<Dungeon>.IsGet*` are all that's needed; after a toggle the menu calls the map
  screen's method 98 (`main+0x33e12c0`, `patch.refresh_screen`).
- **Found by:** GDB read watchpoint on the `IsGetMap` value (`tools/readwatch.py`): the map screen reads it on open
  at `main+0x33e158c` and `main+0xb2a2c4`.

## 6. Things that do not work

Each of these was tried and verified not to help:
- `SeqOneShotResetPlayerStatus` flags (HUD screen `+0x384 = -1`, `+0x388 = 1`): consumed, but no redraw.
- `main+0xe35440(screen)` (the "event end in" routine): no redraw.
- `IsEquipDirect = 1` vs `0`: neither makes the ocarina playable on its own (§5.1).
- Reloading the area (warp): doesn't create the ocarina on Link.
- `ActionGuide_IsEnd.Ocarina` / shortcut tutorial flags: unrelated to the ocarina playing.
- AddPouch with a negative count: does nothing.
- `main+0x2df6c80` with an empty slot: doesn't take gear off.
- AddPouch with `Item_Dungeon_Map/Compass/BossKey/SmallKey`: sets none of the dungeon flags.

## 7. How to investigate

- **Ask the game's scripts first.** `tools/evdump.py` on the relevant `.bfevfl` shows which node and parameters the
  game uses (giving the ocarina, removing an item, opening a chest).
- **Screenshots without bothering the player:** `PrintWindow(hwnd, dc, 2)` on Lichii's main window captures the game
  even when covered.
- **Catch the code that touches something** with Lichii's GDB stub (Emulation > Configure > Debug > Enable GDB Stub,
  port 6543): `tools/dpadwatch.py` (write watchpoints), `tools/readwatch.py <field>` (read watchpoint),
  `tools/magicwatch.py`. Notes:
  - With the stub on, the game is held at boot until a debugger connects; start the script first.
  - **Lichii leaves the game halted when a debugger detaches.** The scripts send `c` and close the socket instead.
  - While a watchpoint is set, that memory page can't be written from outside (`WriteProcessMemory` fails).
  - Turn the stub off afterwards, or the next game start hangs at the launch screen.
- **Snapshot diffs:** `tools/snap.py` (all save fields) or a raw read of an object before/after an action.
- **From an object to its methods:** read its vtable from live memory, map entries to `main+` offsets, then look
  them up with `re64.py`. Functions with no direct callers are usually virtual; search main's data for the
  pointer to find the vtable and slot.
- **Ground rules that saved time:** never write save files (back up with `python flags.py backup`); verify each
  claim with a memory read or screenshot before asking the player to test; warping can mislead tests (§5.8).

## 8. Open problems

- Warping into a dungeon doesn't register the current room, so the map screen shows "???" until you walk through a
  door.
- Upgrades other than the wallet are wired up (§5.7) but not yet confirmed in game.
- Count items (sticks, nuts, bombs, bombchus, beans), bottles and masks aren't cleared from item buttons when
  removed.
- Unticking an ocarina leaves `ActionGuide_IsEnd.Navi` set (the D-pad stays, empty).
