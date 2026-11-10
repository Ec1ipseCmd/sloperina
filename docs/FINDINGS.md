# ootmenu findings

Everything verified while building ootmenu, written so the next person doesn't have to rediscover it. Each solved
problem lists the symptom, the cause, the fix, how it was found, and the approaches that **don't** work.

Applies to **Ocarina of Time (Switch 2), program `0400486024DEC000`, update v131072, main build
`E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`**, running in **Lichii** (an Eden/yuzu fork, Windows). Other versions
have different addresses.

Most of this was found with ootmenu, the Python overlay sloperinaGZ replaced; its files (`game.py`, `patch.py`,
`gamedata.py`, `guestmem.py`, the GDB tools) are named below where they're how something was found. They're
archived outside the project (`ootmenu-python-overlay-2026-11-09.tar.gz`); the native code is in `src/ootgz`.

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
9. [Leads: pausing, debug leftovers, scene managers](#9-leads-pausing-debug-leftovers-scene-managers)

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
  `data/gamedata_fields.json`. About 4,600 names were recovered by hashing every string in the game's scripts and data;
  ~3,600 more fields have readable titles from the onif save editor (`data/all_fields.json`, mapped by diffing exports).
  2,568 more names come from the game's memory (`data/memory_names.json`, see [Names from memory](#names-from-memory)).
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
- **Enum** values are hashes of choice names; the choices are in `data/gamedata_fields.json` (`"v"`).

### Saved or not, and what resets a field
- Every field object starts with the same header: `+0x00` its type's vtable, `+0x08` reset bits (u32), `+0x0C` a
  byte that changes during play (state, not a setting), `+0x10` the **save file index** (s32): -1 = not saved,
  0 = the save slot, 1 to 3 = other save files (`IsLastSelectedMQMode` is 3). Read from all 12,947 Bool, Int, UInt,
  Float, Enum, Vector3 and String64 fields; the counts below were the same in two emulator sessions two days apart.
  Struct, BoolExp, the arrays and the 64-bit types weren't read.
- **Bit 0 of the reset bits looks like "cleared on an area change"**: it's on the gossip stone fairies
  (`BlewOcarinaByGossipStone.GOSSIP_*`, one fairy per visit), battle states (`DekuScrubBrothers_IsBattle`) and
  picked bomb flowers. Inferred from which fields have it, not tested. Bit 3 is on nearly everything, saved or not;
  bits 1, 2, 4, 5 and 8 aren't understood.

Bools by setting:

| Reset bits | Save | Bools | Examples |
|---|---|---|---|
| 0x08 | 0 | 9338 | most progress: `IsGet.*`, `HavePlayedEvent.*` |
| 0x08 | -1 | 444 | `ActionGuide_IsRequestOcarina` |
| 0x18 | 0 | 341 | (unnamed; Ints with 0x18 are `DefeatedEnemyNum.*`) |
| 0x09 | 0 | 288 | `DreamWorld_ArriveFromKakarikoVillage`, `DungeonWarp_Canceled` |
| 0x09 | -1 | 185 | `DekuScrubBrothers_IsBattle` |
| 0x01 | -1 | 128 | `Cow_IsGetMilkFromCow` |
| 0x21 | -1 | 114 | `Npc_DekuTreeChild_GlassTruth` |
| 0x120 | 0 | 102 | (unnamed) |
| 0x19 | -1 | 100 | (unnamed) |
| 0x108 | 0 | 52 | `FairySpiritAppear_HrlField_Note` |
| 0x01 | 0 | 46 | `BlewOcarinaByGossipStone.GOSSIP_*` |
| 0x00 | -1 | 35 | `Sequence_IsAlreadyResetForLoadSave`, `DebugParam.*` |
| other | | 119 | |

- **Saved to the slot and without bit 0 = progress**: 9,869 Bools, 1,119 Ints, 87 UInts, 83 Enums, 76 Floats,
  29 String64s and 12 Vector3s. Checked at a gossip stone: `HavePlayedEvent.GossipStone_D_T` and
  `HaveTalked_Special.GossipStone_Talk_Event3` are saved and kept, `GossipStone_NaviTalk_Temporary` isn't saved.
  The play time counters `0xe573f564` and `0xe53eb6d1` (UInt, +1 a second) are saved too.
- In `data/gamedata_fields.json`: `"r"` (reset bits) and `"s"` (save file index) for those fields. The save fields
  reference shows them and has a **progress only** filter.
- **Found by:** reading every object of the seven type sections from outside the game, grouping them by these
  values and checking the groups against their names.

### Names from memory
- `data/memory_names.json` (`{hash: [name, type, grade]}`) adds 2,568 names. They're merged into
  `data/gamedata_fields.json` (`"ns"` says where each came from; the reference shows it on hover), and
  `tools/re64.py` reads the file after `rom_names.json`. With the ~4,600 from the game's files, 7,161 of the 14,997
  fields are named.
- **How they were found:** every identifier-like string in the running game's heap and in main (about 5 million)
  hashed against the field hashes, plus the game's own patterns filled in with names seen in memory
  (`IsGet.<item>`, `PouchData.<item>.StockNum`, `HavePlayedEvent.<event>`). Mostly `HavePlayedEvent.*` (about
  2,100), then `PouchData.*`, `HavePlayedGetEvent.*` and `IsGet.*`.
- **Grades:** *sure* = a whole string in main, or in a logic graph (AINB) or BYML file within 0x400 bytes of a
  `GameData*` key (for `HavePlayedEvent.X`, a key ending in `EventName`); *likely* = a whole string in a logic graph
  or BYML file, or a name made of words (8+ characters, two words) anywhere else. 98 are sure and 2,470 likely.
  Short or random-looking matches (*unsure*) were left out.
- **Chance matches:** with about 5 million strings against 15 thousand hashes, around 17 collisions are expected.
  Three names in `rom_names.json` were collisions where memory had the real name, and `gamedata_fields.json` now has
  the real ones: `0x4147bf54` `yuzN` is `MiniGame_Diving_ShilverScale_First`, `0xa385357f` `WMb:` is
  `HavePlayedEvent.Npc_Hylia_040_Child_D_N`, `0x54b613cf` `XN1wJ` is `LakeHylia_OverlapSharkAppearArea`. These
  still look like collisions, with no real name found yet: `zSA` (0x82daaab7), `lddC` (0x826db2f9), `6fwL`
  (0xbc8e1794), `iDr4` (0x6310157c), `zsbm` (0x7594791b), `KQigA` (0xe4075522), `f3xXn` (0x833e9356), `kz5L`
  (0xb42e40db), `mooona` (0xf0aad6fa).

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
- **Pause screen layout:** `romfs/UI/Pouch/PouchTable/PouchTable.game__ui__PouchTable.bgyml` (BYML) lists every
  slot of the item, equipment and collection screens with its pouch items: `ItemArray` (item screen, `Pa_ItemIcon_00`
  onward: sticks, slingshot, boomerang, nuts, bombs, bombchus, hammer, hookshot/longshot, bow, fire/ice/light arrows,
  beans, Din's/Farore's/Nayru's, 5 bottles, scrub seed, one trade-item slot, amiibo), `EquipmentArray` (swords,
  shields, tunics, Stone of Agony, Lens of Truth, masks, boots), `CollectArray` (Skulltula tokens, Gerudo card, magic,
  double defense, gauntlets, scale, wallet and bags), `OcarinaArray`, `StoneArray`, `MedalArray`, `NoDisplayArray`
  (deku seeds, heart pieces). Slots holding an upgrade chain list it highest first. ootmenu's items list follows
  `ItemArray`.
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
- **Timing:** there is one request slot, and a request only runs while the game runs (the menu pauses the
  emulator). So every multi-step job goes through one worker, `patch.submit(key, fn, ...)`, which runs jobs in
  order and keeps only the newest pending job per key (scrolling through wallet sizes runs just the last one).
  `patch.wait_handled` waits as long as the game is paused and only gives up after the game has run ~4 s without
  taking the request. Before this, overlapping jobs interleaved on the slot (e.g. two wallet changes removing each
  other's items) and steps were abandoned after 10 s with the menu open.
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

### 5.9 Levitate jerks Link about, can't steer, and once crashed the game
- **Symptom:** holding L3 shot Link upward and yanked him about; he couldn't be moved with the left stick; one
  session ended in a crash. The log showed two "moved Link with record" lines in the same instant, and about ten
  record searches within one second ending in "no record moved Link".
- **Cause 1 (jerks):** releasing and re-pressing L3 while the first press was still searching for Link's physics
  record started a second loop. Both loops checked the shared on/off flag, so both kept writing heights with
  different starting points.
- **Cause 2 (crash):** each overlapping press ran `move_link`'s search, which writes into candidate records. Writing
  look-alike records is what broke the camera and softlocked the game before (see "position" in `game.py`).
- **Cause 3 (no steering):** only the height was written. The game treats Link as falling, and falling Link has no
  air control.
- **Fix:** one loop at a time (`Game._lev_lock` plus a press number that older loops check); a failed search waits
  `LEV_FIND_RETRY` seconds before trying again; the loop writes x, y, z, with the rise easing in and horizontal
  speed set from the left stick relative to the camera (camera forward from `[[main+0x4E98068]+0x68]` +0x38/+0x44,
  right = (-fz, fx)). It stops moving Link when the game's frame counter stops (menu open, emulator paused). The
  controls are now tap L3 to float or drop, hold R3 to keep the height.
- **Found by:** the log; a live test measured each 0.1 s of movement against the camera's forward and right, plus a
  screenshot before and after a move to the right.

### 5.10 Levitate floats Link through walls
- **Symptom:** steering while floating passed straight through walls and rocks.
- **Cause:** the game does resolve collisions for a position we write: between our writes it pushes Link back out
  of a wall. But the float wrote its own running x/z every millisecond, which undid each push-back.
- **Fix** (`Game._float`): each step starts from the game's current x/z when the game has moved Link *against* his
  heading (a push-back), and otherwise from our own position. A push-back marks the wall for `LEV_WALL_MEMORY`;
  while marked, the velocity into it is removed and moves towards it are refused. Push-backs only count once the
  heading has held for `LEV_STEADY_TIME` (0.1-0.2 s). The height stays ours. Settings > "levitate through
  walls" (`levitate_through_walls`, `Game.lev_through_walls`) skips all of this, which is the old behaviour.
- **Why each part is needed:**
  - Starting every step from the game's position also takes in the game's own momentum (it turns our moves into
    velocity at actor `+0x3A0/+0x3A8`): after the stick was let go, Link slid on 2.5 units.
  - Without the wall mark, Link crept about a unit into the rock (hidden inside it). The game's leftover momentum
    into the wall also counted as a push-back once he turned to back off, so he got stuck.
  - Without the steady-heading check, the momentum from the old direction counted as a push-back for about 0.25 s
    after every turn.
- **Found by:** a hover-and-push test into a known rock wall in SacredForestMeadow_Child, from (-5.94, 115.86,
  -106.58) along (0.978, -0.209). The wall is about 10.1 units away. Pushing down into the floor showed the same
  push-back (Link sank about 0.4 and held). Result: Link holds at 9.9-10.1 against the wall through 2.5 s at full
  speed, backs off normally, stops within 0.26 units of the stick being released, and reverses in about 0.25 s
  (that's the ease-in).
- **Physics record motion state** (after the position doubles, f32): `+0x24/+0x28/+0x2C` = requested velocity
  (z, x, y), `+0x3C/+0x40/+0x44` = the same after collision (a blocked component becomes 0); the actor's velocity at
  `+0x388` follows the second copy. The game ignores the position while this is all zero, hence `WAKE`.

### 5.11 Air walls: where they are and what they do
- **Data:** `tools/airwalls.py` builds `data/airwalls.json` from the layouts (`romfs/Banc/<Area>/<Map>_Static|_Dynamic
  .bcett.byml.zs`, the map name is the save's "map" string) and the actor packs (`romfs/Pack/Actor/<Actor>.pack.zs`, a
  SARC). Each placed actor whose name contains `AirWall` has `Translate`, `Rotate` (radians, X Y Z; applied as
  Rz Ry Rx like sead's makeR) and `Scale`. Its shape comes from the pack: PhysicsParam `ControllerSetPath` →
  ControllerSetParam `ShapeNamePathAry` (follow `$parent`) → ShapeParam `Box` (Center + OffsetTranslation,
  HalfExtents) or `Cylinder` (CenterA, CenterB, Radius). What it stops is the RigidBodyEntityParam
  `EnableLayerHitMask`: `HitOnlyPlayer`, `Default` or none stop Link. `HitOnlyCharacter` walls (1565 of them,
  `AirWallEnemy*`) are for enemies and NPCs. 3504 walls in 71 maps; 236 Polytope/PhshMesh/Cone shapes are left out.
- **romfs `.zs` files** use zstd dictionaries from `romfs/Pack/ZsDic.pack.zs` (a zstd SARC): `bcett.byml.zsdic`
  for layouts, `pack.zsdic` for packs, `zs.zsdic` otherwise (`tools/gamefiles.py`).
- **`FldObj_AirWall_Player_Unridable`:** a 4 x 4 x 4 box sitting on its origin (centre +2 up), scaled per
  placement, `HitOnlyPlayer`. In SacredForestMeadow_Child the temple ledge has two:
  - **A (Dynamic):** origin (-6.94, 118.49, -117.60), scale (1.46, 2.87, 2.87). It covers x -9.87 to -4.01,
    y 118.49 to 129.99, z -123.35 to -111.85. It sits on top of the ruin wall in front of the entrance (the screen
    overlay lines up with it).
  - **B (Static):** a 30-unit cube at (-8.67, 122.87, -128.83), tilted 15 degrees about x, above and behind A.
  - Both are in memory once each as f32 position triples (A at `0x215c6ce600`, B at `0x215c658500` this session).
- **Push-out:** while Link floated against A, the requested velocity (record `+0x24`) jumped to +79 to +85 units/s
  out of the wall, and Link left at about 47 units/s, losing it only slowly in the air (wallwatch, `tools/wallwatch.py`).
  It looks like penetration depth over one frame, so the deeper he is when the game resolves it, the harder the
  launch. Which face it chooses hasn't been measured cleanly yet (the overlay's writes were mixed in).
- **The frame counter (`main+0x4E84BA0`) keeps running in the game's menus** (pause screen, R item menu) while
  gameplay is frozen. The levitate used it to hold still while paused, so in the R menu it kept moving Link with no
  collision, which caused most of the launches and the "lost Link's record" stops. It now holds still when the game
  hasn't touched Link's record for `LEV_PAUSED_AFTER` (every gameplay frame moves him off what we wrote).
- **Camera** (`[[main+0x4E98068]+0x68]`): `+0x38` position, `+0x44` look-at, `+0x130` vertical fov (0.8727 =
  50 degrees), `+0x134/+0x138/+0x13C` sin/cos/tan of half of it, `+0x140` aspect (16/9); projection scales at
  `+0x9C` (1/(tan*aspect)) and `+0xB0` (1/tan). Lichii draws the picture in its largest child window
  (`win32.render_rect`).
- **The view** (scene > collision > show air walls): `airwall.py` projects every wall within 60 units with that camera onto a
  pair of see-through click-through windows over the picture: front faces filled on one at the "wall opacity" setting (scene > collision) (default 15%; Tk's `-alpha` and `-transparentcolor` work together), outlines on the other
  at 85% (back edges dashed). Redrawn up to 60 times a second. Cyan = stops Link, red = Link's inside it, violet
  = other kinds. Walls further from the camera than Link get his silhouette cut out (a capsule from his feet,
  `airwall.LINK_SIZE` by age from the map name, filled with the key colour), so he stays in front of them.
  - **Accuracy:** checked against a screen recording while panning (`pan` capture, 160 frames). Link's feet
    project onto his feet in every frame, still or at 95 degrees/s, and the picture trails the camera data by only
    about 0-50 ms.
  - **It can't hide behind scenery:** the overlay has no depth for the game's picture. Doing that needs the map's
    collision (the placed actors' `romfs/Phive/Shape/*.bphsh`, 5877 files, a compressed Havok mesh format, not
    decoded yet) and a ray test per line.
  The "air wall" watch shows Link's depth (physics point) and which face is nearest, relative to the face he went in
  through: back, THROUGH, up, down, side.

### 5.12 Levitate laggy with the air wall view on
- **Symptom:** with scene > collision > show air walls on, levitating (and input in general) felt laggy.
- **Measured** (the real App in a harness pretending Lichii is focused, Link hovering; the levitate thread's
  position writes counted):
  - View off: 240 writes/s, longest gap 6.5 ms, 1.3 ms per Tk tick.
  - View on: 88 writes/s with one **2.4 s stall**, 3.9 ms per tick (the stall didn't happen on every run).
  - The emulator's own frame rate was unaffected (51 frames/s with or without a 60 Hz see-through overlay).
- **Cause:** the levitate thread shares the process (and the GIL) with the Tk thread, which redrew every wall
  from scratch each tick. With Lichii in the foreground Windows favours it, so the overlay's threads get less
  CPU and the levitate loop misses its slots; while it isn't writing, gravity pulls Link down.
- **Fix:**
  - The process runs above normal priority (`win32.raise_priority`) and the levitate thread at highest
    (`win32.raise_thread_priority`); `sys.setswitchinterval(0.001)`.
  - The view caches each wall's world-space corners, projects each corner once, reuses canvas lines
    (`airwall.Painter`), redraws at most `AIRWALL_FPS` (30) times a second, and checks the picture's position
    twice a second.
  - The "inside a wall" test only runs when the view, its watch or the menu is showing.
  - Result: view on 168-176 writes/s, longest gap 11 ms, 1.4 ms per tick.
- **Also found:** the landing heal checked health once, 0.6 s after touching down, and once missed the damage
  (6 quarter hearts). It now watches for `LAND_HEAL_WINDOW` (1.5 s).

### 5.13 Moving Link from the menu
- `Game.teleport(x, y, z)` writes the physics record (`+ PHYS_Y`) followed by `WAKE` when the record is known,
  so it works while the game is paused: the record keeps what was written and the move shows when gameplay runs.
  `_record_ok` (record within 3 units of the drawn position) fails in between, so `_record_is_ours` also accepts
  a record that still holds our last write. While floating, the float loop takes the position (`_lev_jump`);
  during a soft landing the landing is ended first (its curve would pull him back).
- If the record isn't known yet, `move_link` finds it, which nudges Link and needs the game running.
- **The dive meter** isn't a save field (only "Diving depth", the scale) or one of the parameter objects on
  Link's status object (`[[link+8]+0x6A0]`: only health `+0x1C10` and max health `+0x1C18`, each listed twice,
  `+0x1CD8/+0x1CE0`). `tools/linkwatch.py` records his actor, status object and every object they point to
  (394 objects, 816 KiB) to find it from a dive.
- **What it isn't (so far):** `[link+0x6F8] +0x188..+0x194` follows the meter (0..1) but is a short history of
  gauge values for its animation, rewritten every frame (writing it does nothing). The ~4,100 objects two
  pointers out from Link hold 16 more 0..1 copies; holding each full during a dive didn't slow the meter. Code
  storing to +0x18C/+0x190/+0x194 together (`main+0x33ED40`, a 4x4 matrix normalise; `main+0x27BD5DC`, a
  vector copy) isn't it either. `tools/meterscan.py` now scans the whole heap (5.3 GiB) in batches while the
  meter drains, saving progress in `tools/meterscan_state.json`.

### 5.14 After a warp, a fresh attach found the wrong memory
- **Symptom:** after a menu warp to LostWood_Child, a newly started tool (or ootmenu after a restart, or after
  the save data went missing for 3 s) reported "save data not found" and no Link, while the game was running.
- **Cause:** `_find_base` scans the process from low addresses for the self-referencing value at
  `main+CHECK_OFF`. Lichii also keeps the guest RAM as a flat backing store (the 14 GiB block `guestmem` uses),
  where the same value matches. That page had become readable, so it was found first: guestbase `0x4e1000`,
  main `0x1ae892d2000` instead of `0x2304c200000` / `0x8073a000`.
- **Fix:** `_valid_base` also needs Link's vtable (`main+VT_LINKOBJ_OFF`) to start with a function inside main.
  Vtable entries are absolute guest addresses, so this only holds in the real mapping.

### 5.15 Levitate didn't lift Link off the ground (Lost Woods)
- **Symptom:** L3 started the float (no errors) but Link stayed on the ground.
- **Cause 1:** the frozen-gameplay check (§5.11) only watched the record. Standing, the game snaps Link to the
  ground without touching the record (his drawn height flickers), so after the first small lift the float
  decided gameplay was frozen and stopped rising.
- **Cause 2:** standing, the game also clears the motion state after the position (`WAKE`) and then ignores the
  record altogether. `WAKE` was written once at the start; in the meadow Link was airborne before it was cleared,
  in the Lost Woods it wasn't.
- **Fix:** gameplay counts as running if either the record or the drawn position changed; the first
  `LEV_START_GRACE` (0.3 s) never counts as frozen; the float rewrites `WAKE` whenever it finds it cleared.
  Tested in LostWood_Child: about 2.4 units in 1.2 s, then a normal landing.

### 5.16 Black screen after warping from the Minuet pad
- **Symptom:** a menu warp to SacredForestMeadow_Child / ForestMinuet made from inside the meadow, with Link on
  the Minuet pad, left the screen black for good (HUD too) while the game kept running.
- **Cause:** auto-exit puts Link into the nearest exit, and the nearest of the meadow's two exits is the
  Forest Temple door, 1.6 units from the pad (the other is the Lost Woods one, 205 away). The warp swapped the
  door's destination for the meadow: the door's own transition started (fade out) but never got its arrival,
  so the fade-in never came. After that, loading zones did nothing either (moving Link into the Lost Woods exit
  with a new warp armed didn't fire it).
- **Fix:** `arm_warp(..., keep_into=DUNGEONS)`: if an exit's original destination is a dungeon, the warp leaves
  it alone ("skipped", Link goes into the dungeon normally) and the menu remembers that exit for the session so
  auto-exit picks another one next time.
- **Not checked:** whether other door-like exits (houses, shops, grottos) behave the same.

### 5.17 Upgrade choices (magic meter etc.) jumped between values
- **Symptom:** inventory > magic > magic meter wouldn't settle on 0 / 240 / 480; one burst of presses logged
  "= 0" then "= 480" in the same second and ended with neither magic item held.
- **Cause:** upgrades change the pouch through the game's own code (`patch.set_upgrade`), which only runs while
  the game runs; with the menu open the emulator is paused, so the request waits. The choice read the value back
  from the pouch, i.e. the old one, so every press stepped from the old value and queued a stale change.
- **Fix:** while a request for that setting is queued or running (`patch.pending`), the choice shows and steps
  from the value last chosen; the queue keeps only the newest request per setting, so the last choice is what
  gets applied. Applies to every upgrade choice (magic, wallet, bags, quiver).

### 5.18 How the game changes maps; warps to warp-song pads crashed (native ootgz)
- **Mechanism:** an exit is a `FieldAndDungeonEntranceEventRequestActor` (map layouts) that requests event
  `Dm_SY_BancChangeGo` with `NextBanc`, `NextStartPos`, `NextStartPosAir`, `BootEventName` (the arrival
  event, from its `NextEventName`) and `IsCheckAir`. The flow's `GameSystemActor.EventReserveBancChange`
  is an AI module (`romfs/Pack/Actor/GameSystemActor.pack` › `AI/ReserveBancChange.module.ainb`) that
  only writes GameData: `Sequence_ReservedNextBanc`, `…NextStartPosName`, `…BootEventName`,
  `…EventStarterName` (String64 indexes 0x22, 0x23, 0x20, 0x21: these are game.py's `next_map`, `spawn`,
  `event`, `starter`), `…NextPosition`, `…DungeonRoomID`, `Sequence_BancChangeFadePattern`, and
  `Sequence_ControlInEvent = ChangeBancOnEvent`, which the Event sequence (`romfs/Sequence/Event.module`)
  turns into `ChangeBancOnEvent` › `ChangeBanc`. The native menu requests the same event the way the
  `SeqOneShotRequestEvent` node does (`main+0x32db1a0`; layout in `z64hd_v131072.h`). Verified: instant
  warps to Kakariko and Hyrule Field.
- **Symptom:** warping to `SacredForestMeadow_Child / ForestMinuet` crashed after the game reserved the map.
- **Cause:** `Dm_SY_BancChangeGo` always reserves the spawn name as the *event starter*, and the standard
  arrival (`Dm_SY_BancChangeArrive`) uses that actor. At exit spawns the exit actor has that name; at
  warp-song pads there is only a `StartPos`. The game enters the pads from `Dm_SY_Ocarina` › `OcarinaWarp`
  with `EventStarterName` empty and `BootEventName = OcarinaWarp_Arrive` (blue warps and sage rooms also
  use an empty starter, with story events).
- **Fix (not yet confirmed in game):** `tools/spawnstarters.py` records, per spawn, the actors that share
  its name (`data/spawn_starters.json`: 145 of 336 have the exit actor, 191 have none). Warps to spawns with
  none pass `OcarinaWarp_Arrive` and clear the reserved starter as soon as the game has reserved the map.
- **Found by:** dumping every event flow's `EventReserveBancChange` (`evfl`) and comparing the layouts of an
  exit spawn and the Minuet pad.

### 5.19 Copies of Link's position; the native levitate (in progress)
- **A copy, not the record:** Link's actor `[+0x8]` → `[+0x458]` → `[+0x410]` → `+0x288` holds (x, y, z) doubles 0.585 above
  his drawn position, followed by a second (x, y, z) about 1 unit lower. A pointer search from the actor (depth 4)
  finds this and one more like it. **The game rewrites it from the real record every frame:** a per-frame trace
  of writing a rising y showed the game's value back each frame (wrote 115.338, read 115.062). Writing it does
  nothing lasting.
- **Crashes:** game.py's 48-byte WAKE written after one of these copies (over the second position) crashed the game,
  twice. So did test-writing loosely matched look-alikes (1.0 tolerance, 12 matches). Nothing is now written past a
  position unless the bytes there are all zero or plausible floats, and doubles there mark a copy.
- **Now:** levitate scans the heap like game.py (16 MiB per frame, regions nearest the actor first) for a match
  within 0.3 that is followed by float motion state, not doubles, and test-lifts each (at most 6). Not yet
  confirmed in game.

## 6. Things that do not work

- **Using character 0x7F for an icon:** menu, log, splash, and input panels already use it as an
  invisible sizing marker. The 0.0.3 JS Height padlock made those markers visible throughout the UI.
  In 0.0.4 the 0x7F glyph is blank and the padlock uses 0x80. The real menu layout and font are covered
  by a host regression test that checks sizing markers stay blank; the menu and watch were also
  rendered headlessly. In-game verification of the correction is still pending.

Each of these was tried and verified not to help:
- `SeqOneShotResetPlayerStatus` flags (HUD screen `+0x384 = -1`, `+0x388 = 1`): consumed, but no redraw.
- `main+0xe35440(screen)` (the "event end in" routine): no redraw.
- `IsEquipDirect = 1` vs `0`: neither makes the ocarina playable on its own (§5.1).
- Reloading the area (warp): doesn't create the ocarina on Link.
- `ActionGuide_IsEnd.Ocarina` / shortcut tutorial flags: unrelated to the ocarina playing.
- AddPouch with a negative count: does nothing.
- `main+0x2df6c80` with an empty slot: doesn't take gear off.
- AddPouch with `Item_Dungeon_Map/Compass/BossKey/SmallKey`: sets none of the dungeon flags.
- Link's velocity on the actor (`+0x388/+0x38C/+0x390`): output only. Writing `+0x38C` (y) every millisecond while
  he stood still kept the value but didn't move him, and the game set it back to 0 afterwards (§5.9).
- Steering by writing velocity instead of position (§5.10), tested while hovering at 4 units/s for 1 s each:
  - Record `+0x24/+0x28` (requested velocity) and actor `+0x388/+0x390`: no movement.
  - Record `+0x3C/+0x40` (after collision): moves Link only about 25% of the asked amount (the game rewrites it
    every frame), and it comes after collision, so it can't respect walls anyway.
- Zeroing actor `+0x3A0/+0x3A8` (the game's horizontal momentum) every ms while steering: Link went straight
  through the wall. The push-back is carried by that velocity.
- Writing the `Sequence_*Pause*` GameData values (`GamePauseRequestCount` at 1 for 2 s, `PreGamePauseRequestCount`
  at 1 for 20 s): nothing happens. They only keep count for the pause graphs (§9.1). **Don't leave one changed**
  (debug > pause research can): at 1, the game's own pause screen and menus would skip their real pause request.

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

### Native collision bypass (0.0.9 test build)

- **Symptom confirmed by the user in 0.0.8:** some walls pass, others block Link or trap him.
  The session log shows the record returning to y=110.463 while the mod requests heights from
  108.701 to 112.102, and drawn y remains 109.623. `frozen` remains zero in those attempts.
  The pause heuristic therefore does not explain this particular blockage. Disabling only the
  mod's push-back response never disabled the game's collision solver.
- **Verified by disassembly and read-only live inspection:** the character property resolves as
  `[ [ [ [ [link_actor()+0x270]+0x50]+0x20]+0x10]+8 ]`, each bracket a u64 dereference.
  Its vtable is main+0x4C34F10 (`CharacterProperty`); +0x20 points to the controller body,
  vtable main+0x4C34658. These are module offsets, not session addresses.
- `OneShotPhysicsCctChangePosition` at main+0x2900120 follows this chain and calls property
  vtable +0xE8 = main+0x6B2530 with a pointer to three f32 coordinates. This preserves the
  rotation, changes the transform translation, then invokes the attached-body updates through
  vtable +0x250 = main+0x6B23A0. The controller transform translation and Link's drawn position
  matched in live reads. The scanned double record uses the capsule centre; the mod measures
  its vertical offset from drawn feet when floating starts rather than assuming child/adult height.
- `OneshotChangeForceNoHitMode` (name getter main+0x295F800, executor main+0x295F0D0) reaches
  that same body through property +0x20 at main+0x295F564..574. Its collision toggle calls
  main+0x158D40(body, bool). That function locks the body and queues the filter change. The
  current state is body +0x70 bit 11, overridden by `[body+0x60]+0xF8` bit 0 when that pending
  object's +0x100 bit 19 is set. The setter preserves the other filter fields.
- **Implementation:** `cct.c` validates the current actor/controller types, attached lists and
  pending state; remembers the prior collision mode; calls ForceNoHit while through-walls
  levitation is active; and uses the game's position setter alongside the existing physics
  record. L3, disable and mode changes restore the prior collision state. Unavailable objects
  defer restoration until they can be validated; replaced actors/controllers are never written.
  The frozen-state shortcut can no longer bypass the physics record's mapping check.
- **Validation boundary:** native build and host lifecycle/integration tests pass. Live reads
  verified the chain/types and static analysis verified the call paths. The emulator build ID matches the installed test module. With the option enabled, a live
  read confirms body +0x70 bit 11 becomes set and the controller transform matches drawn position.
  The user subsequently confirmed traversal through the previously blocking wall. The first retest had
  through-walls off (log and saved setting both confirmed), so it did not exercise the bypass. The release fall fix is documented below. Camera behavior
  is not separately verified. The old stationary-position pause heuristic remains.

- **Manual vertical controls (fork 0.0.8):** X commands ascent, B commands descent, neither/both
  hold height; R3 overrides both. At a legacy saved speed of 0, manual movement uses 4 units/s.
  With through-walls enabled, B descent bypasses the mod's upward floor correction, which would
  otherwise undo downward movement every frame. X/B/R3 are masked from the game only while floating.
  Host tests verify direction/release/reversal, floor feedback, simultaneous horizontal movement,
  menus, legacy zero speed and mask cleanup. This is not a collision-mask change: wall/ceiling
  blocking, camera follow and the stationary-position pause heuristic remain unresolved. No game
  runtime verification of the controls has been performed.


- **Fast fall after L3 release (fork 0.0.7):** the user reports a very fast fall when stopping
  levitation. Previously `drop()` stopped position writes without clearing any physics velocity.
  At that stage stored falling velocity was suspected but unconfirmed. That release cleared only the
  documented requested/post-collision y velocities (`record +0x2C/+0x44`, §5.10), once, after checking
  the record still follows Link, is readable/writable and has plausible motion state. It does not
  change position, horizontal velocity or gravity. Host tests cover the exact writes, L3/disable,
  invalid records and no further writes after release. The game may have additional momentum state;
  the user has now confirmed that 0.0.7 still drops too fast. Clearing these fields is insufficient;
  that attempt was insufficient. See the verified 0.0.9 follow-up below.

- **Zero-rise hover (fork 0.0.6):** rise speed previously offered a minimum of 2, and settings
  rejected 0. The menu now offers 0 and settings retain it. At zero rise, vertical levitation velocity
  is explicitly cleared: simply adding 0 to the options would leave a previous upward velocity
  unchanged because the acceleration step is also 0. Host tests verify stationary hover without R3,
  horizontal wall passage at fixed height, switching from positive rise to 0 and back, and persistence.
  Floor corrections from 0.0.5 still apply; the game's runtime behavior remains to be tested.

- **Levitation floor support (fork 0.0.5):** `float_step` read the game's current y but then wrote
  its own accumulated y back, discarding upward physics corrections even while passing through walls.
  It now retains the higher of those heights before adding the requested rise. Horizontal wall
  push-back remains controlled by the existing option; downward drift does not lower the hover.
  Host tests exercise floor corrections, rising platforms, both wall modes, pause/drop and record loss.
  This is not a floor raycast or a change to collision masks: an upward change can also be an impulse,
  and missing geometry or a floor crossed between physics updates cannot be detected here. Actual
  wall/floor behavior, slopes and moving platforms still need in-game verification.

- **JS Height reset/lock (0.0.3):** the user supplied the f32 chain
  `[[link_actor() + 0x270] + 0x358] + 0x18E8` and reported `FLT_MAX` on fresh load, with jumps/sidehops
  replacing it with takeoff Y + 0.70. The watch reads it dynamically; optional reset writes `FLT_MAX`
  once and lock repeats that write in `watches_frame`. Both resolve the current actor and check
  alignment, overflow and read/write mapping before writing only four bytes. Lock is session-only,
  defaults off, and continues independently of HUD visibility. Its interaction with the game's
  threshold setter/check ordering has not been runtime-verified. A per-frame freeze cannot guarantee
  suppression of a setter and check that run between mod frames; the game's setter would need separate
  investigation if that occurs.

- Warping into a dungeon doesn't register the current room, so the map screen shows "???" until you walk through a
  door.
- Upgrades other than the wallet are wired up (§5.7) but not yet confirmed in game.
- Count items (sticks, nuts, bombs, bombchus, beans), bottles and masks aren't cleared from item buttons when
  removed.
- Unticking an ocarina leaves `ActionGuide_IsEnd.Navi` set (the D-pad stays, empty).
- Collision view (gz's): shelved. The collision files are decoded (`tools/phive.py`); the plan, costs and format
  are in [COLLISION.md](COLLISION.md).
- Frame advance freezes the picture while paused (MANUAL §7). The game's own pause is a request count in the pause
  manager, and one type stops the world while the game keeps drawing (§9.1).
- Leads in §9 not followed up yet: a player debug move state that may survive (levitate without searching for the
  physics record), the game's own debug line and triangle shaders (collision view, stage 2), and the scene's
  managers (a Link state name, the exit Link is nearest to, the map's collision, the explored room after a warp).

## 9. Leads: pausing, debug leftovers, scene managers

Found from outside the game (memory reads and test writes in Lichii, static analysis of main) and not used by
sloperinaGZ yet. Same build as everything else here.

### 9.1 Pausing: the pause manager
- **The `Sequence_*Pause*` values only keep count.** Opening the map (Minus) or the pause screen (Plus) sets
  `PreGamePauseRequestCount` and `IsPreGamePause` to 1, about 60 ms later `GamePauseRequestCount` and `IsPause` to 1
  with the pre request back to 0; closing puts both back to 0 with a one-frame `PostGamePauseRequestCount` and
  `IsPostGamePause`. A dialogue changes none of them. They belong to the sequence graphs `ChangeGamePause.module`,
  `ChangePreGamePause.module` and `ChangePostGamePause.module`: on input `IsPause` they add or take 1 from the count,
  at 1 run `SeqOneShotRequestPause` with `PauseType` `GamePause`, back at 0 `SeqOneShotReleasePause`, and below 0
  raise an app error ("[Seq]GamePause underflow, please report"). Callers: `GamePauseOn`/`Off`,
  `PlusMinusPauseOff`, `OcarinaPauseOn`/`Off`, `ShortCutPauseOn`/`Off`.
- **The pause manager** is `*[main+0x4e97e38]`: `+0x00` the number of pause types (20), `+0x08` the type table,
  sorted by ID, 0x20 bytes per type: `+0x00` name, `+0x08` ID (MurmurHash3 of the name), `+0x0C` and `+0x10`
  bitmasks (probably what the type stops), `+0x1C` the request count (bit 31 is a separate flag; keep it). `+0x48`
  is a listener whose notify function (`main+0xafba60`) is an empty `ret`.
- `SeqOneShotRequestPause` (vtable `main+0x4c6e208`, work in slot 39 at `main+0x3bb4260`) turns `PauseType` into an
  ID and calls `main+0xafb980`, which finds the type, adds 1 to its count atomically and notifies the listener.
  Release is the next function, `main+0xafba70`. So a pause is just a count above 0, and raising one from outside the
  game works like the game's own request. From inside, those two functions are the ones to call (their arguments
  weren't traced).
- The 20 types: `ResetGimmickFromScript`, `LoadingPause`, `UpdateBancResource`, `DebugPause`, `SceneBeginning`,
  `EventPlayMovie`, `EventDisableController_Camera`, `EventSetupOrRestore`, `WarpPause`, `PostGamePause`,
  `SystemPauseWhileLoading`, `UpdateResourcePause`, `EventBancChangePause`, `ResetGimmick`, `PreGamePause`,
  `GameOverPause`, `GamePause`, `EventDisableController`, `SceneTransition`, `ClickPause`. IDs: `GamePause`
  `0xba597383`, `EventDisableController` `0xd468c7d1`, `EventDisableController_Camera` `0x5cf23aab`, `DebugPause`
  `0x2d67e2da`, `ClickPause` `0xfbad2d6a`.
- **Tested in Lichii**, a count raised to 1 from outside and back to 0:
  - `EventDisableController`: Link ignores the controller while the world keeps moving (gz's menu behaviour). It
    was raised and lowered for each of 16 menu opens in one session, while the game's own events raised and lowered
    it too, with no trouble.
  - `GamePause`: everything stops like the pause screen and the game keeps drawing, but some objects disappear while
    it lasts (the game's pause screen hides that behind a captured picture).
- For frame advance (§8): `GamePause` stops the world without stopping the picture, with the missing objects as the
  catch. `DebugPause` and `ClickPause` look like debug leftovers and haven't been tried; either may be the
  developers' own stop.

### 9.2 Debug leftovers
A name in main doesn't mean the feature still works: release builds keep the names of registered nodes and
parameters while the code behind them is compiled out.
- **Scene selector: almost certainly compiled out.** RomConfig (`sInstance` at `main+0x4e97e60`) is filled from
  `System/RomConfig/%s%s` (`main+0x3bc1230`) and `System/GameRomConfig/%s%s` (`main+0x9b9a90`), with the build name
  picked from `Debug`, `Develop`, `Pilot` and `Product` by an index hard-coded to 3 (Product). The reader
  (`main+0x9b9de0`) stores `BootupScene` (+0x8) and `UseSceneSelector` (a byte at +0x118). The reflection type's
  constructor defaults `UseSceneSelector` to 1 and the Product config sets 0, but nothing reads +0x118: the boot only
  checks whether `BootupScene` is empty (scene manager init `main+0x3ba9fb0`, update `main+0x3baa670`), and no
  string names a scene select screen. Not checked: whether romfs also holds Debug or Develop RomConfig files.
- **Debug drawing: the game side is compiled out, the renderer side isn't.**
  - Every `ExecuteDebugDraw*` and `ExecuteDebugEntry*` node keeps its parameter list, but its per-frame method is a
    bare `ret` (`ExecuteDebugDrawBox`, slot 29; the same for `DrawSphere`, `EntryCapsule`, `EntrySphere`, `AppLog`
    and others).
  - `ExecuteDebugEntryRigidBodyShape` still has a body. It asks `main+0x38bb7b0` to draw each `RigidBodyConst`,
    `RigidBodyEntity` and `RigidBodySensor`, which loads the physics object `X` (`*(*(main+0x4c7df00))`, then
    `+0xC0`) and returns at once if `X+0xE0`, the debug draw manager, is null. It always is: the physics setup
    (`main+0x1206010`) creates X's members at +0xD8 and +0xE8 to +0x108 but nothing at +0xE0.
  - Still there: the render layer table (`main+0x4c0bab0`: `CalcView`, `CalcGPU`, `BG`, `Opa(Debug)`, `Opa`, `Xlu`,
    `Xlu(Debug)`, `PostEffect`, `DebugInfo`, `FinalImage`, `Text(Debug)`) and the system shader list
    (`main+0x4c04608`: `debug_primitive`, `debug_shape_instanced`, `debug_point_instanced`, `debug_line_instanced`,
    `debug_triangle_instanced`, `debug_colored_mesh`). These are the candidates for COLLISION.md's stage 2, route 1
    (a shader the game has already built). Whether they're loaded at run time wasn't checked.
  - `phive_debug_draw` is the water simulation's height field view, not collision. Havok's Visual Debugger viewers
    (`hknpShapeViewer`, `hknpBroadPhaseViewer` and others) are linked in; whether the VDB server starts wasn't
    checked.
- **Player debug move: may survive.** `ExecutePlayerDebugMove` and `ExecutePlayerSubDebugMove` are registered
  nodes, and Link's behaviour tree has `SelectorPlayerSchedulerDebug` and `SelectorPlayerSubSchedulerDebugMove`.
  Unlike the draw nodes, `ExecutePlayerDebugMove` isn't empty: slot 29 (`main+0x27d3e60`) follows `[this+8]`,
  `+0x8`, `+0x1F0`, `+0x270`, `+0x358` to a player component and calls `main+0x933270` on it with request 0xF. Not
  traced further. From inside the game it can simply be tried; if it's a free-move state, it's a levitate that
  doesn't need Link's physics record (§5.19).
- Other names: `FocusDebugCamera` (compared as a command name at `main+0x3b54b34`); the scene parameters
  `DebugBootScript`, `IsChildWhenChangeBanc` and `IsForceSwitchChildOrAdult`; the rendering switches
  `dev_colorization`, `dev_scope_draw`, `dev_color_bars`, `dev_lkt_ui` and `tera_develop`;
  `SeqExecuteResetGameDataAllForDebug`; `engine__debug__ClickMenu` (no references). Not found: anything named cheat,
  noclip, god mode, level or stage select, or ImGui.

### 9.3 The scene's managers
`S = *[*[main+0x4e97648]+0x1F0]` (the scene manager's current scene) holds 41 component pointers at `*[S+0x58]`
(count at `S+0x50`). Each component's vtable has a name getter (`adrp x0; add x0; ret`) among its first 24 slots.
Read in Kokiri Forest:

| Entry (offset) | Component | Entry (offset) | Component |
|---|---|---|---|
| 0 (+0x000) | Sequence (`FormalSequence.root`) | 22 (+0x0B0) | WarpMgr |
| 5 (+0x028) | PlayingInfoMgr | 23 (+0x0B8) | OptimizeChangeBancMgr |
| 6 (+0x030) | GlobalGameParameterTable | 24 (+0x0C0) | SystemCullingMgr |
| 7 (+0x038) | BancLoadMgr | 25 (+0x0C8) | DropMgr |
| 8 (+0x040) | LocationMgr | 26 (+0x0D0) | LimiterActorCountMgr |
| 9 (+0x048) | NavMeshMgr | 27 (+0x0D8) | CompoundNavMeshObjectMgr |
| 10 (+0x050) | StaticCompoundBodyMgr | 28 (+0x0E0) | GameSafetyMgr |
| 11 (+0x058) | PhiveVolumeStatsMgr | 29 (+0x0E8) | AreaCullingMgr |
| 12 (+0x060) | AutoSaveMgr | 30 (+0x0F0) | RouteRailMgr |
| 13 (+0x068) | LoadProgressMgr | 31 (+0x0F8) | DungeonMgr |
| 15 (+0x078) | ResidentActorMgr | 32 (+0x100) | OcarinaMgr |
| 16 (+0x080) | BakeLoadMgr | 33 (+0x108) | OcarinaCreationMgr |
| 17 (+0x088) | OwnedHorseMgr | 34 (+0x110) | NaviMgr |
| 18 (+0x090) | MinigameMgr | 36 (+0x120) | NpcMgr |
| 19 (+0x098) | TimerMgr | 37 (+0x128) | AgonyStoneTargetMgr |
| 20 (+0x0A0) | LoadingPauseMgr | 38 (+0x130) | FoliageLoadMgr |
| 21 (+0x0A8) | AmiiboMgr | 39 (+0x138) | EventSubstituteStarterMgr |

Entries 1 to 4 and 14 were empty; 35 and 40 had no name getter.

- **NaviMgr keeps a copy of Link's transform** every frame for Navi (the game's `QueryNaviGetPlayerPosAndRot`
  reads it): a row-major 3x3 rotation at +0x894 (yaw = atan2(+0x89C, +0x8B4)), the root position (his feet) at
  +0x8B8, the position at +0x8D0 and the velocity in units per second at +0x8DC. The chain held through loading
  zones, a death, the title screen and a game reboot. It's only a copy: writing it doesn't move Link.
- Leads, not followed up:
  - AutoSaveMgr `+0x1DC` holds a short name that follows what Link is doing: `Move`, `Wait`, `EvtTalk`, `EvtWait`,
    `EvtLookAroundAttention`. A start for a Link state watch.
  - WarpMgr (`+0x170`, a flag at `+0x148`) and OptimizeChangeBancMgr (`+0x80`) hold the destination of the exit Link
    is nearest to; in the Lost Woods they switched between `GoronCity_Child`, `KokiriForest_Child` and
    `LostWood_Child` as he walked.
  - StaticCompoundBodyMgr is the obvious place to look for the map's collision in memory (COLLISION.md, "the
    unknowns"), and DungeonMgr for the explored room a menu warp leaves unset (§8).



### 9.4 L3 release: controller velocity and landing history (0.0.9-test2)

- Read-only observation of the running 0.0.9-test build found body `+0xE0` velocity
  `(0, -60, 0)` while Link hovered; actor output `+0x388` and momentum `+0x3A0`
  also reported approximately -60 Y. Clearing only scanned-record `+0x2C/+0x44`
  in 0.0.7/0.0.8 did not reset the actual controller (confirmed by the user).
- `ExecutePhysicsCctReduceVerticalGravityPositionVelocity`, helper main+0x278AA90,
  reads body+0xE0 and calls main+0x12DBE0 with a new vector. This setter locks the
  body and queues linear velocity at pending+0x3C with pending+0x100 bit 7. The
  mod now calls it each floating frame and once at release, preserving applied
  X/Z and clearing Y. Collision restoration follows the release reset. Gravity
  is not altered and velocity is not clamped during the subsequent fall.
- `QueryPhysicsCctIsOnGround` at main+0x2AABA20 reads property+0x48, selects the
  result pointer at +0x40 + 8*[+0x58], and tests result+0x10 == 0. This is used
  to end landing protection after six supported frames, rather than a timer.
- main+0x5FCF6C computes player+0x190C = player+0x18D4 - actor+0x320; +0x5FD128
  maintains the peak Y at player+0x18E0. +0x5FD19C feeds actor's base component
  `[[actor+0x270]+0x2B8]` at +0x1898. main+0x484A10 computes its fall distance
  +0x189C = max(origin Y - actor Y, 0). During levitation and its first landing,
  these origins follow current Y and distances are cleared. The base backpointer
  +0x1340 and player backpointer +0x18 must match the current actor before writes.
  JS Height +0x18E8, health, gravity and actual position are untouched by this reset.
- The old 90-frame health refund was removed. It could expire during a long drop
  and restored damage after it happened. The new mechanism resets fall history;
  it does not suppress enemies or other damage and does not provide void immunity.
- Host tests pass for terminal-speed reset, preserving horizontal motion, null/
  unavailable/replaced objects, exact fall-history targets, long drops, supported
  landing, and ending protection. Native v131072 build passes without warnings.
  **User runtime result: gentle fall, no damage.** The test2 log independently
  shows the release reset queued, collision restored, and normal fall damage restored after
  landing. This is a confirmed test case, not exhaustive coverage of every scene/event.
- User confirmed the preceding test's native through-walls bypass passes the wall
  that previously trapped Link, after turning through walls ON. Void-trigger
  visualization and optional levitation void immunity are still under investigation.


### 9.5 Enemy life and live actor enumeration (0.0.10-test)

- The actor/process manager is `**(main+0x4C7DC40)`. Its circular intrusive list has a sentinel
  at manager+0x68 and nodes at actor+0x190 (next, previous). The list also contains system processes;
  only objects with actor vtable `main+0x4B25E18` are treated as actors. Name is actor+0x20.
  Read-only inspection found hundreds of entries; guards check back-links, bounded traversal and types.
- `QueryLifeGetLifeByActor` (executor 0x2A487A0) reaches the base component through actor+0x270,
  component-table+0x2B8. Its actor back-pointer is +0x1340. Native getters 0x2D0BCC0 and 0x2D0BCF0
  read parameter pointers at +0x1C10 / +0x1C18; values are signed 32-bit at parameter+8, with
  parameter vtables 0x4B63438 (Link implementation) and 0x4B4C400 (ActorLife implementation).
  Live reads observed ReDeads 12/12, Deku Babas 2/2 and a Gold Skulltula 1/1.
  The new watch selects living `Enemy_` actors, not every object with a life component.
- The position-calculator component is table+0x128, vtable 0x4B47058, owner+0x18. The native query
  selects attachment vectors including +0x44, +0x5C, +0x74 and +0x80 (selector 0x20B370).
  The label uses the highest finite nearby attachment plus 0.35, falling back to actor root+1.85.
  These are upper attachment positions, not a verified universal head-bone index.
- Camera projection is shared with air walls. Game-thread reads publish at most 32 nearest labels;
  NVN copies a bounded mod-owned snapshot and never traverses actor memory. Missing/changed lists clear
  labels. No cached per-enemy addresses or game-memory writes are used by Enemy Health.
- Host tests and production label rasterization pass. After accepting the ActorLife parameter type in
  test2, live published labels changed from `2/2` to `1/2` and the user confirmed ReDead labels appear.
  Boss coverage and other enemy-specific behavior still require runtime tests.

### 9.6 Void sensors and selective suppression (0.0.10-test)

- Void sensors are separate from solid collision. Area actors expose their logic binder through
  `[actor+0x3D8]+0x80`; binder vtable 0x4B25A60, its node pointer at +8, Area node vtable 0x4C5B298.
  Node+8 is the logic context; context+0x60/+0x68 is the node count/pointer array. An Area's
  SensorHitInfo output cell is node+0x80. Abyss consumer input at +0x28 must reference that exact cell:
  simply finding a dangerous node elsewhere in the same logic module causes false positives.
- Native consumer vtables: GiveAbyssActor 0x4B90830, conditional variant 0x4B909B0,
  SendSandAbyss 0x4B91E08. The standard handler returns death reason 8 (`FallIntoAbyss`, also
  verified in the native enum's ordered string table). Conditional nodes read reason through +0x78,
  target-name parameter through +0x68, and activation through +0x60. Null target name permits all;
  a non-null name must match Link. A non-null empty string is not an unrestricted target.
- A live Market Area was linked to the conditional node but targeted Blue Fire, used reason 10,
  and had a false condition. It must **not** be shown as a player void. This confirmed why classifying
  every GiveAbyss node or generic Area as a void is incorrect.
- `QueryPhysicsGetRigidBodySensorByIndex` (helper 0xC8BCD0) follows the physics component at
  `[[actor+0x270]+0x50]`, +0x20 controller set; +0x38 counts sensor controllers, +0x40 points to
  their array. First controller+8 is a body pool (count+0x10, entries+0x18, stride 0x18, body+8).
  Sensor body vtable is 0x4C37D20; body+0x28 is shape, body+0x80 is a row-major 3x4 transform.
- The cached-bounds shape implementation at 0x38614B0 uses scale+0x10, local half-extents+0xC0,
  centre+0xD0. The viewer checks this virtual-method identity and reads only those supported shapes.
  The sampled sensor's extents, centre and transform agreed with its actor position. Curved shapes use
  bounding boxes; the viewer does not reproduce the solver's tiny contact-margin expansion.
  Unsupported controllers/shapes are counted unavailable. Indirect/aggregated logic links and global
  cleanup/scripted events are not inferred. No extracted geometry or logic resources are shipped.
- Suppression is an opt-in, session-only mode. The game-thread frame publishes the current Link actor
  ID (+0x3E8) only when the selected mode applies. The death-message sender 0x2654B20 copies a 12-byte
  payload and uses target ID/category at target+0/+4. The hook rejects only Link, category 0x613B0000,
  reason 8. Every other message retains all original arguments and the original return value.
  A separate hook at sand sensor call 0x31E6A54 filters only Link/category 0x76910020, forwarding other
  calls to 0x373A20. It does not turn off unrelated deaths, damage, actor cleanup or scripted warps.
- IPS v2 is 143 bytes in seven records. The sender-entry fallback at cave+0x70 executes the displaced
  PACIBSP then branches to sender+4, preserving native return authentication. The frame and sand stubs
  use the original-function fallback. Additional slots +0x4E9A4D8/+0x4E9A4E0 were zero in live reads,
  beyond the declared MOD0 BSS end and within the mapped NSO BSS, like the existing frame slot.
  Boot checks reject mismatched patch versions/sites or occupied slots.
- Host tests validate selection/conditions, invalid memory, rotated bounds, modes and message forwarding;
  generated-IPS tests decode branches, pointer slots and fallback instructions. Native build and headless
  menu/label checks pass. **The new hooks, void outlines and immunity have not been runtime verified.**

### 9.7 Loaded static collision and stable previews (0.0.10-test3)

- The user reported missing health labels, only invisible barriers in Collision, and no general terrain
  controls. The health reader had accepted only Link's parameter vtable. Native ActorLife construction
  at main+0x2E43AAC installs main+0x4B4C400; it uses the same value-at-8 layout. Both verified parameter
  types are now accepted with the existing actor ownership and read guards. ReDead labels are user-confirmed.
- Native main+0x6F31F0 resolves static collision through scene `[[main+0x4E97648]+0x1F0]`,
  component array +0x58, component 10 (+0x50). StaticCompoundBodyMgr vtable is main+0x4BA1FE8.
  Its 16 streaming slots start at +0x38, stride 0x48; the map-wide slot is +0x6F0. Slot+0x30 is the body
  (vtable main+0x4C324F0), body+0x10 is resource (main+0x4C32600), resource+8/+0x10 hold blob and size.
  Mapping, size, identity and owners are rechecked each frame. No extracted collision assets are distributed.
- The bounded decoder follows phive/TAG0 relative arrays, compound transforms, meshes, convex hulls,
  boxes and cylinders. Mesh vertex buffers may have padding; rejecting a remainder incorrectly omitted
  otherwise valid geometry. Nested ground containers may place their transform section after the shape;
  locate it before decoding. Player mask bit 3 selects solid collision, material 27 excludes AirWall,
  and entity layer 8 excludes Rough. Static sensor layers 13/14 are optional general triggers, not
  automatically voids. Face normals split floor/wall/ceiling. Capsules/spheres remain unsupported.
- On one loaded Castle Town tile the C decoder emitted floor/wall/ceiling/trigger totals
  73456/118144/35459/72. The reference's additional 32 solid triangles were unsupported capsules.
  A full scan and 4096-primitive batches matched in 31 batches. User confirmed test2 geometry appeared.
- Test2 replaced completed geometry with partial nearest-256 snapshots every four frames, and spent
  the rectangle budget on the nearest edges. These code paths explain the reported flicker and tiny
  visible area. Test3 publishes only complete scans, keeps 2048 unique preview triangles, offers a
  fixed world-grid simplification (or exact vertices), hashes shared edges and distributes drawing cost
  over the whole projected set. The existing total rectangle limit stays 3000. Trigger coordinates
  are not simplified. A captured-tile replay covers the full selected 12-unit radius with 463 grid-1
  triangles or 870 exact triangles. Synthetic regressions check snapshot stability and distant-edge
  rendering under a dense foreground. Test3's in-game appearance/performance are not yet verified.
- Void Areas can expose multiple sensor controllers. Inspect up to 16 independently instead of requiring
  exactly one. An unsupported controller does not hide other valid controllers. Keep direct player-void
  logic association checks; the status explicitly says when no identified sensors are loaded.
- Levitation now stops at loading flags Sequence_IsLoadingBanc (0xEF2B60C2) and
  Sequence_IsChangingBancFromRequestToPause (0xF8633C7F), map-name change or actor replacement/loss.
  Clear active levitation, search and landing state before touching the departing record. Do not invoke
  release movement on the arriving actor. The saved L3 feature setting remains enabled. Synthetic tests
  cover same-map reloads and transitions; runtime transition behavior remains unconfirmed.
- The user also reported "Link's position not found" while standing still. That notice comes from the
  legacy levitation heap search/test lift, not the collision viewer. The available log showed record
  loss and successful reacquisition, not the reported failing attempt. It also showed an adult record
  centre offset of 0.840 versus the legacy search's assumed 0.585 (within, but close to, its 0.3 tolerance).
  Do not treat these observations as proof of the exact failed attempt's cause.
- Through-walls levitation now starts from the validated drawn position and uses the existing native
  controller position setter directly. It no longer needs a heap match, assumed centre offset, or test
  lift. X/B and stick steering, queued velocity reset, fall-history protection and load cancellation
  remain. Native hover checks actual game pause flags rather than mistaking a stationary player for a
  frozen game. Legacy collision-respecting movement and 2x speed retain their search; its failure text
  now identifies the movement record. Host tests cover no-scan startup, stationary hover/resume,
  ascent/descent, pause/menu, release and invalid/replaced objects. Native-only movement requires runtime testing.

### 9.8 Broken collision lines (0.0.10-test4)

- The user reported the collision view looked normal but its lines were very broken. Test3 distributed
  a limited number of rectangles over all edges by skipping pieces of each staircase/dotted line.
  This preserved coverage but deliberately introduced gaps; increasing the range made them more apparent.
- Surface edges now rasterize end-to-end into a bounded color mask. Equal horizontal runs are merged
  across rows into rectangles and submitted through the existing scissor-clear renderer. No new shaders,
  game assets, graphics API imports, gameplay writes or settings are needed.
- Start at 640x360. If the complete mask cannot fit, retry at 320x180, 160x90, then 80x45. Only a complete
  packed image is published; never sample fragments from its lines. The surface budget is 8000 rectangles,
  the shared outline buffer 12000, and command memory 2 MiB per slot (previously 256 KiB), retaining
  command-free-space guards. Coarser images can thicken/merge nearby outlines; this is a bounded preview,
  not a full-resolution GPU wireframe renderer. Original void/barrier line style is unchanged.
- Host tests verify connected components/endpoints for line slopes/directions and boundary/zero-length
  cases, exact reconstruction of the packed mask, clipping, colors, distant edges and buffer bounds.
  A synthetic 3003-edge scene selects 320x180 and 3419 rectangles; every raster pixel survives production
  NVN rectangle drawing. Headless simple and dense images were inspected. Runtime appearance and the
  performance impact of the larger drawing budget still need user confirmation.


### Abyss reaction still starts with sensor suppression (0.0.10-test5)

Symptom: the user reports a void reaction while levitating even though Link's position survives.
The existing hooks cover abyss/sand messages, not all native abyss classification paths.
Static analysis found the shared classifier at `main+82C510`: its only nonzero Player+191C store
is `main+82C690`, followed by the global abyss notification at `+82C6A8`. It selects types
1, 3, 4, 5, 7, 8 and 9. These are abyss classifications, NOT the death-message reason enum.
The scheduler at `+82BC08` and event path at `+27D4E50` call it and immediately dispatch using
Player+191C; the event query at `+2A3827C` reads the derived +1C38 bit 5.

Patch v3 intercepts the state commit. For the protected Link only, it branches to the classifier's
native epilogue (+82C614), before state/global notification, instead of restoring a position later.
Unprotected/unrecognized cases replay the original store and continuation. The predicate verifies
Player+18's actor type/ID and the reverse actor+270 -> +358 chain. It changes no native data.
The shim preserves w8 and LR; continuation/epilogue analysis verifies other volatile registers and
NZCV are dead. Existing message filters remain. Interceptions publish an atomic bitmask; the frame
callback coalesces notifications to at most once per 120 frames, with a HUD notice and log entry.
Bits 1/3/4/5/7/8/9 are classifier types; bits 16/17 are abyss/sand messages.

A read-only check against the running process confirmed the original instruction at the new site
and an empty hook slot. Host checks cover ownership, all emitted types, mode changes, untouched
native objects, notification throttling and IPS fallback/branch targets. This is not confirmation
that the user's exact trigger is fixed in-game. Already-running events and unrelated scripted
warps are not cancelled. Test after installing both files and restarting; enable protection before
entering the trigger, check there is no void camera/fade/respawn, then return to safe ground and
verify normal mode and ordinary area transitions still work.


### Collision left disabled after L3; tower death-volume visibility (0.0.10-test6)

Read-only inspection of a user-reproduced stuck state found the current CCT body with
force-no-hit still set, ground state 1 (unsupported), and vertical velocity -60 after
levitation was off. The log showed the same property selecting different bodies, followed
by a later activation saving `was 1` and release restoring 1. The former owner comparison
included the selected body: a posture/body switch was mistaken for controller replacement,
so the original body was never restored. Its disabled flag became the next baseline.

The property remains stable while +20 selects among the typed CCT bodies in its attached
list (+58 -> count +10, pointer array +18). Keep a bounded per-session baseline for each
body touched. Accept body selection changes within the same actor/property, and restore all
still-attached, revalidated bodies on release. Actor/property replacement still forbids stale
writes; unreadable controllers are retried. A body whose collision was already disabled
before this session keeps that state. Restart after installing to clear a state left by an
older build; this fix does not force arbitrary existing no-hit states off.

Also seed native hover from the CCT's actual 3x4 transform (+80 translation +0C/+1C/+2C),
with the pending transform (+60, flag +100 bit 6) taking precedence as in native setter
+6B2530. The drawn position can differ from physics during animation: one stationary sample
was about 0.072 units below the physics origin. Using it could move the capsule into a floor.

At tower collapse, three direct player-abyss Area associations had valid box bounds. Two
volumes were 200 units across. Twelve perimeter edges do not visualize a boundary whose
entire perimeter is off-screen. Add bounded grids across the actual six faces, keeping the
active/inactive colors. A conditional abyss node without a direct sensor input was NOT used
to classify every Area sharing its graph as a death barrier.

Missing tower scenery also has ordinary entity bodies, outside StaticCompoundBodyMgr.
Read entity controller count/array at physics controller +18/+20, controller +8 pool,
pool +10 count/+18 entries, 24-byte entries with body at +8. Verified entity body vtable
+4C378C0; body +28 mesh wrapper (+4C31DE8), +80 world matrix. Wrapper +10 scale,
+80 holder -> +8 root, +A0 resource (+4C31EE0). For inline resources only, descriptor
at resource-48 has vtable +4C31F00, bytes +8 and size +10; require the root to lie inside
those bytes and root+30 to point back to the resource. Constructor +3864670 establishes
that inline layout; independently allocated resource clones are intentionally skipped.
Standalone shape sections use the existing relative-array decoder and player-material filter.
Collect up to 128 actor shapes in addition to 17 static slots. Capture one transform per
complete scan, retain source bounds/type checks each frame, and reuse existing triangle budgets.

Validation: synthetic body-switch A/B/A restoration, original flag preservation, stale-owner
and unavailable-controller guards; queued/physical origin and animated-position regression;
actor ownership/root/file bounds and read-only tests; standalone shape transform/scale/filter
and malformed-input tests; giant-volume headless render. This establishes the original runtime
fault and source-level regressions, not successful runtime behavior of the new build. The game
closed before the new module was installed. Dense-line coarsening, actual 50% opacity and
visible-surfaces-only rendering remain separate renderer work.


### Void lines did not communicate the danger area (0.0.10-test7)

The user confirmed that lines were present, but the dangerous region was unclear. Test6's
8-unit grid (capped at 32 subdivisions) left large close-up gaps and offered no distance or
inside indication. This is a visualization change, not a broader claim about detected hazards.

Project and clip the complete sensor-box faces against the near plane and all four frustum
sides. Draw continuous contours plus fixed screen-space red hatching for active bounds;
inactive bounds are grey. Outside a box, draw the camera-facing faces; inside, draw the faces
visible from the interior. This avoids perimeter-only failures and hidden back-face clutter.
Use a bounded 4000-rectangle budget. Draw active void overlays after ordinary collision and
inactive bounds. Minimum outline width no longer follows menu font size. Hatching is opaque
with gaps; it is NOT alpha blending. General dense-surface raster coarsening is unchanged.

Compute the nearest point on each recognized active oriented box by projecting Link's displayed
position onto its orthogonal axes. Outside: clamp to the box; inside: select the nearest face.
An inside warning takes priority over proximity to a separate box. Reject invalid positions,
degenerate/skewed bounds; ignore inactive sensors. Distances are to sensor bounds, not a guarantee
of safe clearance for Link's capsule. Curved sensors still use conservative bounding boxes.

A small mod-owned snapshot feeds a persistent bottom-center warning and boundary cross. Off-screen
nearest points show a direction; no identified active bounds is stated explicitly. The graphics
thread performs no game-memory reads for this feature. Toggling show voids off or pausing clears
the hint. Existing visibility settings and void suppression behavior are preserved.

Host regressions cover rotated and overlapping boxes, inside/outside/face/corner distances,
inactive/missing/invalid data, giant-volume and near-face projection, rotated view clipping,
shared drawing limits, warning states and actual NVN text rendering at multiple scales. Headless
approach/inside/unmapped views were rendered and inspected. No runtime appearance or avoidance
claim is made for the new build. No game assets or new rendering dependencies are included.
