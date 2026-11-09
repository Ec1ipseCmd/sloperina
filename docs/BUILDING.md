# Building ootgz (the native, in-game version)

sloperinaGZ (source: `src/ootgz`) is ootmenu rebuilt the way gz is built: C code that the game loads
and runs itself, on its own thread, once per frame. No emulator attachment, no Python at run time.
The Python overlay it replaced (`ootmenu.pyw`, `game.py`, `patch.py` ...) is archived outside this
folder (`ootmenu-python-overlay-2026-11-09.tar.gz`); the tables below still name its files.

## Prerequisites

- [devkitA64](https://devkitpro.org/wiki/Getting_Started) (`DEVKITPRO` set; provides
  `aarch64-none-elf-gcc` and `elf2nso`) and a host C compiler, **or**
- Docker: `./docker-make` runs the Makefile in the `devkitpro/devkita64` image.

## Building

```
make            # or ./docker-make
make dist       # or ./docker-make dist
```

Outputs, per game version (only `v131072` exists so far):

| File | What |
|---|---|
| `bin/v131072/subsdk9` | the module (NSO), added to the game's exefs |
| `bin/v131072/E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips` | IPS32 patch for main (143 bytes) |
| `dist/` | install trees, see [INSTALLATION.md](../INSTALLATION.md) |

The link fails if the module imports a symbol that isn't in `lib/imports.txt` (the functions main
and sdk are known to export). A function we call but forgot to define would otherwise link as an
import and only fail when the game loads the module. Add a symbol there only after checking that
main or sdk exports it.

### JS Height host tests

`python tests/run_js_height.py --cc C:/msys64/mingw64/bin/gcc.exe` runs the watch, settings, and
HUD code on a 64-bit Windows host. With GCC on PATH, use `python tests/run_js_height.py` instead.
It checks the pointer chain and f32 formatting, compatibility with old settings, HUD visibility,
one-shot reset, lock/unlock, read-only/unavailable targets, exact four-byte writes, and relocation.
It also draws a real menu and checks that invisible panel-sizing markers do not become lock icons.
The game and SD filesystem are mocked; this does not verify the game's setter/check ordering.

### Levitation host tests

`python tests/run_levitate.py --cc C:/msys64/mingw64/bin/gcc.exe` runs the real levitation step/frame
logic with a simulated physics record. It checks upward floor corrections, downward drift, wall
push-back handling, hover, rising platforms, X/B ascent/descent, direction reversal, both-button/R3
hold, manual descent past upward floor corrections, button masking, menu/drop and record loss/restart.
Release tests cover the controller velocity reset, guarded fall-history reset, protection through long drops,
and ending protection after supported landing. Invalid records are skipped. These tests
do not simulate the game's collision solver; verify surfaces and transitions in-game separately.

The same command also runs `tests/cct.c`: dynamic controller resolution, exact object types,
queued collision state, preserving an existing no-hit mode, restoration/retry, replacement,
invalid lists and non-finite positions. Its game API calls are mocked. The user confirmed the 0.0.9 test build passes a previously blocking wall
and test2 gives a gentle L3 fall with no landing damage. Logs show the queued velocity reset and
protection ending after landing. Other surfaces, events and scene transitions need broader testing.


## Layout

Mirrors gz (`gz/src/gz`, `gz/lib`, the Makefile's version list):

| Path | gz equivalent | What |
|---|---|---|
| `src/ootgz/z64hd.h`, `z64hd_v131072.h` | `z64.h`, `lib/liboot-*.a` | game addresses for one version, as main offsets |
| `src/ootgz/ootgz.c` | `gz.c` | module init, frame hook, menu tree |
| `src/ootgz/crt0.S`, `lib/ootgz.ld` | `hdr.S`, `ldr.S`, `gz.ld` | module header (MOD0) and linker script |
| `src/ootgz/menu.c`, `gfx.c` | `menu*.c`, `gfx.c` | menu model; text screen shared with the renderer (log fallback) |
| `src/ootgz/nvn.c`, `font.c` | `gfx.c`, `res/` fonts | drawing over the game's picture through its NVN; gz's Press Start 2P font |
| `src/ootgz/got.c` | | redirects main's imports (NVN, nn::hid) |
| `src/ootgz/input.c` | `input.c` | controller input via the game's `nn::hid` |
| `src/ootgz/gamedata.c` | (z64 save context) | GameData fields (port of `game.py`) |
| `src/ootgz/pouch.c`, `job.c` | `gz_inventory.c` | inventory through the game's own code (port of `patch.py`) |
| `src/ootgz/cheats.c`, `link.c` | `gz_cheats.c` | cheats; Link, camera, frame counter |
| `src/host/mkips.c` | `genhooks`, `gru` scripts | writes the IPS patch from the same address header |

## How it loads

Two files, both of which LayeredFS (Atmosphère) and Lichii/Eden's `load/` folder can add
without touching the game's files:

1. **`subsdk9`**: the game's exefs has `rtld`, `main`, `subsdk0` (movie) and `sdk`;
   `subsdk1`-`9` are free. rtld loads every module present, links imports by name and calls
   each module's `DT_INIT`. ootgz imports `nnMain` from main (to find main's base: no memory
   scanning) and `nn::hid`, `vsnprintf`, `memcpy`... from sdk. It exports nothing.
2. **The IPS** uses seven records (patch format version 2):
   - `main+0x3c46400`: marker `OOTGZ\0\0\0` + patch version (the module checks it)
   - `main+0x3c46440`: a 5-instruction stub: load the pointer in `main+0x4e9a4d0`; if set,
     jump to it, else run the original GameData unlock
   - `main+0x3b500dc`: the per-frame "unlock GameData" tail call → the stub (the same site and
     cave the old patch used, FINDINGS §3)

   - `main+0x2654b20`: the death-message sender entry branches through the stub at `main+0x3c46460`.
     Its fallback executes the displaced `PACIBSP`, then resumes at sender+4. The module filters only
     Link's `FallIntoAbyss` reason; other targets/reasons retain the original call and arguments.
   - `main+0x31e6a54`: the sand-abyss sensor's call branches through `main+0x3c46480`, with the
     original sender as fallback. The two additional slots are `main+0x4e9a4d8` and `main+0x4e9a4e0`.

   `ootgz_module_init` (DT_INIT) checks the marker, version, all hook sites and empty pointer slots,
   then stores the frame and message callbacks in their slots. The stub falls back to the original code if the
   module is missing, so the IPS alone is harmless.

The module runs exactly where the old assembly hook ran: the game's thread, once per frame,
with GameData just flushed and unlocked. So everything `patch.py` did through the mailbox
(AddPouch, remove, gear, screen refreshes) is now a direct call; multi-step changes run one
step per frame through `job.c` because SetInt is queued until the next flush (confirmed from
the disassembly of `main+0x163b70`).

What changed from the Python version:

| Python (emulator) | Native |
|---|---|
| finds guest base + main by scanning Lichii's process | `&nnMain - 0x1202cd0` |
| finds GameData Int/Bool/Enum/String tables by heap scans | type entries inside the GameData manager (`**[main+0x4c7e180]`, Int entry at `+0xC8`; layout from GetInt) |
| one mailbox request per frame, polled by a worker thread | direct calls; `job.c` keeps one step per frame |
| XInput/keyboard on Windows | `nn::hid::detail::GetNpadStates` from the game's sdk |
| Tk overlay window | `gfx.h`: log backend now, on-screen backend next |
| equipment component / magic meter: heap scan | same idea, in-process (`mem_find_u64`), after a cheap search near Link |

## Verified in Lichii (2026-11-09)

- rtld loads `subsdk9` and calls `DT_INIT`; the IPS applies; the frame hook runs
  (`ootgz: frame hook installed`, `ootgz: running (patch v1, main 0x805e3000)`).
- Input through `nn::hid::detail::GetNpadStates` works: R + right stick down opens the menu,
  A enters a submenu.
- String64 fields (map name `Market_Child`), Link's health (80/80) and drawn position read
  correctly. No crash.
- Not yet tried: upgrades, ocarinas, cheats, input interposition, the slot watchpoint.

## Verified statically

- The module builds with no warnings; three page-aligned segments, MOD0 at rodata start,
  `DT_INIT` = `ootgz_module_init`, only `RELATIVE`/`GLOB_DAT` relocations, nothing exported.
- All 9 imports exist in the game: `nnMain` in main; `memcpy memmove memset snprintf vsnprintf`
  and the three `nn::hid::detail::GetNpadStates` overloads in sdk (checked against the
  modules' dynamic symbol tables).
- The IPS disassembles to the intended stub; its site branch is byte-identical to the old
  patch's `SITE_BYTES`.
- `murmur3` matches the Python one (`SimpleIntForMessage3` = `e670579a`,
  `Sequence_CurrentBanc` = `1d6189da`).
- The slot `main+0x4e9a4d0` is past the bss end MOD0 declares (`0x4e9a4c8`), inside what the NSO
  header maps (`0x4ea0000`), and zero in two live dumps.

## Unverified: check these first, in Lichii

Lichii runs the game's own rtld, so a working load there is good evidence for hardware.

1. **The module loads and the hook runs.** Remove the old `load/0400486024DEC000/ootmenu`
   patch first (same site). Turn on Lichii's log with the emulated debug output category at
   Debug level (yuzu-family logs `svcOutputDebugString` under `Debug.Emulated`). Expect
   `ootgz: frame hook installed` at boot and `ootgz: running (patch v1, main 0x…)` once in game.
   If nothing appears, check whether rtld calls `DT_INIT` for subsdk modules on this system,
   and whether `__nx_module_runtime` (0x200 bytes) is enough for the Switch 2 rtld.
2. **The slot is really unused.** Set a write watchpoint on `main+0x4e9a4d0`..`+8` with the
   GDB stub (as `tools/dpadwatch.py` does) and play a few minutes. Only ootgz should write it.
   If the game does, pick another address and change `Z64HD_HOOK_SLOT`.
3. **Input.** Open the menu with **R + right stick down**; the log prints the menu. If nothing
   reacts, check the `npad_state_t` layout in `input.c` (buttons at +8, attributes at +0x20).
4. **GameData types.** Bool/Enum/Float entries are assumed to share the Int entry's layout
   (table size at `+0x24`); String64 is found from the `Sequence_CurrentBanc` pair the way
   `game.py` does it, but searched among the manager's type entries. The link menu shows the
   map name if strings work.
5. **Features.** Compare with ootmenu: upgrades (wallet first, it's the verified one), ocarina
   give/remove, the cheats.
6. **Drawing and hidden input** (see "Drawing" below): expect `NVN hook installed`,
   `input hook installed`, then on the first menu or notice `swapchain: N textures, WxH,
   format F` and `drawing on screen: ...`. Any `on-screen drawing off: <why>` line says what
   failed; the menu then keeps going to the log.

## Drawing

- **Hooks.** At module init, `got.c` rewrites main's GOT slots (found through main's MOD0 and
  dynamic section) for `nvnBootstrapLoader` and the five `nn::hid::GetNpadStates` overloads.
  Checked offline: the slot found for `nvnBootstrapLoader` is `main+0x4cbf710`, the one main's
  PLT stub at `main+0x3c42a90` jumps through. This doesn't depend on rtld's search order.
- **NVN.** The game gets `nvnDeviceGetProcAddress` from the bootstrap loader and fetches every
  function by name through it (loader at `main+0x129e000`), so the wrapped GetProcAddress
  swaps in our `nvnQueuePresentTexture`, `nvnWindowBuilderSetTextures` and
  `nvnDeviceInitialize`. Before each present the menu panel (rasterised on the CPU, in the
  swapchain's format) is copied into the texture being presented with
  `nvnCommandBufferCopyBufferToTexture`. No shaders, which Switch 2 would need built with
  non-public tools. The panel is opaque.
- **Checked against the game:** `WindowBuilderSetTextures` is called as `(builder, count,
  textures)`; `TextureBuilderSetFormat` gets 37 (RGBA8), 1, 13, 61 (RGB10A2) and
  `MemoryPoolBuilderSetFlags` 0x21/0x12/0x22/0x24/0xA1, all NVN 1 values. Not checked: the
  present call's arguments (no direct call site found) and the signatures of the pool,
  command buffer and sync functions (NVN 1's are used). Supported swapchain formats: RGBA8,
  RGBA8/RGBX8 sRGB, RGB10A2, RGBA16F.

## Ported from ootmenu

link (health, rupees, position), warps (all places, arrival cutscenes, cancel), cheats,
inventory (items, ammo, capacity, magic, bottles, upgrades, quest items, songs, dungeon items,
small keys, skulltulas), equips (worn gear, owned gear, masks), story (quest stages, tutorials),
watches (on-screen, timer, lag counter, gz's R + D-right / R + A + D-left / R + B + D-left /
R + B + D-right). The menus' data (field hashes, stages, warps) is `src/ootgz/data.c`,
generated from the JSON files by `python tools/gendata.py` and committed, so the build needs no
Python.

Warps start at once (unconfirmed in game): the module requests the event an exit requests,
`Dm_SY_BancChangeGo` with `NextBanc` / `NextStartPos` / `NextStartPosAir` / `BootEventName` /
`IsCheckAir` (found in the map layouts: exits are `FieldAndDungeonEntranceEventRequestActor`,
`FirstEventName` = `Dm_SY_BancChangeGo`). That flow's `GameSystemActor.EventReserveBancChange` is an
AI module (`romfs/Pack/Actor/GameSystemActor.pack` › `AI/ReserveBancChange.module.ainb`) that writes
the `Sequence_Reserved*` GameData (String64 indexes 0x20-0x23 are `ReservedBootEventName`,
`ReservedEventStarterName`, `ReservedNextBanc`, `ReservedNextStartPosName`) and sets
`Sequence_ControlInEvent` = `ChangeBancOnEvent`, which the Event sequence turns into the map change.
The request is built like the `SeqOneShotRequestEvent` node does (`main+0x32db1a0`): constructor
`main+0xd79f10`, params as `QueryBancChangeEventRequestArg` fills them (`main+0x29bf380`), submit
`main+0xd7a820(**[main+0x4c7e090], &request)`; layout in `z64hd_v131072.h`. If the game hasn't
reserved the map within 90 frames, the warp falls back to swapping the next exit Link takes.

## Next steps

1. **Not ported yet**: x/y/z editing and teleport, steering while levitating (Link's physics
   record is now found by `levitate.c`: a pointer search from his actor plus a test lift; the path
   it logs can become a direct chain), the air wall view, custom memory watches.
2. **Levitate / teleport** need Link's physics record from a pointer chain, not a search
   (`game.py` "position").
3. **Air wall view**: needs 3D line drawing (a shader, or lines rasterised on the CPU); last.

## Installing

See [INSTALLATION.md](../INSTALLATION.md).

## Enemy Health and voids test build

Build and install with `make dist` (or `./docker-make dist`), then replace **both** mod files and
restart the game. Patch version 2 is required; a v1 IPS paired with this module is rejected.
No original game file is an input to compilation or included in the mod package.

Additional host checks (pass `--cc /path/to/gcc` when needed):

```
python tests/run_enemy_health.py
python tests/render_enemy_health.py
python tests/run_voids.py
```

These cover actor-list replacement/corruption, health validation, pause/visibility, bounded projection,
read-only memory access, void sensor association and reason/target filters, rotated bounds, mode changes,
and exact IPS branches/slots/fallbacks. Label rendering uses the production NVN drawing function and font
with a headless rectangle backend. This does not emulate the native message queue or game physics.

Runtime checks still required for this test build: add Enemy Health in Watches, damage an enemy and
confirm current decreases while max remains fixed; reload the scene and restart to check the saved watch.
Enable Scene > Collision > show voids near a known pit. Check the bounds and summary, then use
void-outs = off while levitating and cross the same trigger with L3 floating active. Return to safe ground
and normal mode to verify restoration. Visibility persists; suppression returns to normal after restart.
Read-only field validation and host checks are not in-game verification of these new features.
