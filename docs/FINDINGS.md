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
  Stored falling velocity is a suspected cause, not runtime-confirmed. Release now clears only the
  documented requested/post-collision y velocities (`record +0x2C/+0x44`, §5.10), once, after checking
  the record still follows Link, is readable/writable and has plausible motion state. It does not
  change position, horizontal velocity or gravity. Host tests cover the exact writes, L3/disable,
  invalid records and no further writes after release. The game may have additional momentum state;
  the user has now confirmed that 0.0.7 still drops too fast. Clearing these fields is insufficient;
  the underlying controller/collision state remains unresolved. Do not describe this as a verified fix.

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
