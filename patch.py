"""ootmenu's code patch for the game (gz-style hooks run by the game itself).

Lichii (Eden) applies IPS32 patches from
    %APPDATA%\\eden\\load\\0400486024DEC000\\ootmenu\\exefs\\<main build id>.ips
when the game starts. Offsets are into the main module image plus its 0x100-byte header.
Patches can't be applied to a running game: Lichii translates and caches game code.

Code space: the gap between the end of main's code (0x3c461d0) and rodata (0x3c47000) is
mapped executable and zero-filled. Lichii's own fixes use 0x3c46200-0x3c46278; ootmenu uses
CAVE_START.. onwards.

The menu installs/updates the file on start and checks the live game's memory to see whether
the current patch is active (the patched bytes are visible there).
"""
import os
import struct
import time
from pathlib import Path

GAME_ID = "0400486024DEC000"
BUILD_ID = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348"
NSO_HEADER = 0x100
CAVE_START, CAVE_END = 0x3C46400, 0x3C47000

VERSION = 12
# Marker: proves the patch is loaded (never executed).
MARKER_AT = CAVE_START
MARKER = b"OOTMENU\0" + struct.pack("<I", VERSION) + b"\0" * 4

# --- give-item hook -------------------------------------------------------------------------
# Every frame, main+0x3b500a4 locks GameData, flushes its command queue (main+0xb99980) and
# tail-calls the unlock (main+0x3c42470) at SITE. The site is redirected to HOOK, which:
#   unlock; v = GetInt(SimpleIntForMessage3); if v >> 16 == TAG: SetInt(.., 0) and, for item = v & 0xff,
#   AddPouch(*[main+0x4c7e0d0], &name, 0, 1, 0, 0, 0, IsEquipDirect = v >> 8 & 1, [sp]=0)   (main+0x110d570, called the
#   way the OneShotAddOrRemovePouchContent node used by the Saria scene calls it), then its
#   notify main+0x110d0b0(*[main+0x4e6b718], &name, ok, 0), then refreshes the HUD the way the
#   SeqOneShotResetPlayerStatus node (main+0x32ddf40) does (PlayerStatus screen +0x384 = -1, +0x388 = 1).
# GetInt = main+0x163d20(mgr, &out, hash), SetInt = main+0x163b70(mgr, value, hash), mgr =
# *[main+0x4c7e180]. Name: {char* top; s32 size=0x80; char buf[]} on the hook's stack.
# Assembled with keystone (tools/hook_asm.py); bytes kept here so the menu needs no assembler.
MAILBOX = 0xE670579A  # SimpleIntForMessage3: Int, never saved. The game also writes it (save-date text,
# main+0x33cc94c), so requests are tagged: TAG << 16 | remove << 9 | IsEquipDirect << 8 | item (1..N, or
# 0x7F = call). remove: main+0x33a4fa0(pouch manager, &name, 1, 0, 0, 0), what the game's own
# AddOrRemovePouchContent node does for Num < 0 (AddPouch with a negative count does nothing).
TAG = 0x0CA7
ITEMS = ["Item_Ocarina_Fairy", "Item_Ocarina_Time",
         # (gear names: in the patch since v9, unused by the menu; the player equips gear in the pause menu)
         "Weapon_Sword_Kokiri", "Weapon_Sword_Master", "Weapon_Sword_Giant", "Weapon_Sword_BrokenGiant",
         "Weapon_Sword_Biggoron", "Weapon_Shield_Deku", "Weapon_Shield_Hylian", "Weapon_Shield_Mirror",
         "Armor_Adult_Goron", "Armor_Adult_Zora", "Armor_Adult_HeavyBoot", "Armor_Adult_HoverBoot",
         # upgrades: the game shows the pouch item (and keeps the wallet name), not just the capacity
         "Item_Wallet_Small", "Item_Wallet_Medium", "Item_Wallet_Large", "Item_Scale_Silver", "Item_Scale_Gold",
         "Item_StickBag_Small", "Item_StickBag_Medium", "Item_StickBag_Large",
         "Item_NutBag_Small", "Item_NutBag_Medium", "Item_NutBag_Large",
         "Item_BulletBag_Small", "Item_BulletBag_Medium", "Item_BulletBag_Large",
         "Item_BombBag_Small", "Item_BombBag_Medium", "Item_BombBag_Large",
         "Item_Quiver_Small", "Item_Quiver_Medium", "Item_Quiver_Large",
         "Item_FairyBless_MP", "Item_FairyBless_MPDouble", "Item_FairyBless_DefenseDouble",
         "Armor_Child_Gauntlet_Goron", "Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold",
         # dungeon items: the game applies these to the dungeon Link is in (chests give them this way)
         "Item_Dungeon_Map", "Item_Dungeon_Compass", "Item_Dungeon_BossKey", "Item_Dungeon_SmallKey"]
# PlayerStatus.Equip* String64 each item goes in (game.STR_IDX keys). Swords/shields/tunics/boots
# don't go through the patch: the player equips those in the pause menu (the game's own equip code).
EQUIP_STR = {"Item_Ocarina_Fairy": "ocarina", "Item_Ocarina_Time": "ocarina"}
SITE, HOOK = 0x3B500DC, 0x3C46440
SITE_BYTES = bytes.fromhex("d9d80314")
HOOK_BYTES = bytes.fromhex(
    "fd7bb3a9fd030091f35301a9f55b02a908f0ff97d5810090b5c240f9a00240f9e01100b4ff3300b9e1c3009142f38a52"
    "02cebc722b761497f33340b913110034687e1053e99481521f01096b81100054a00240f90100805242f38a5202cebc72"
    "b4751497741e1812731e00127ffe017121050054a00240f9ff3700b9e1d30091421e8b52c2cdb87216761497a00240f9"
    "ff5300b9e143019162ae8752829caa7210761497a00240f9ff5700b9e1530191221e9a52c240ac720a761497a00240f9"
    "ff5b00b9e163019142c9955242a4a97204761497a00240f9ff5f00b9e173019182d59d528217a272fe751497a8f8ff10"
    "09888cd28978a0f2080109cbe93740b90801098be02b40f9e12f40f900013fd653000014530a00347fb20071080a0054"
    "090b00106a06005129150a8bea330191ea2300f90b108052eb4b00b92c3540a94c3500a92c3541a94c3501a95f810039"
    "c8810090086940f9000140f9000800b4e1030191f4004836220080520300805204008052050080527a7add9713000014"
    "0200805223008052040080d2050080520600805287220853ff4300d1ff030039e41b5397ff43009102000012289100b0"
    "08611c91000140f9200500b4e1030191030080d2ab1a5397c8810090084d40f9140140f9540400b4802640f9080040f9"
    "082940f981878352e18fab7200013fd6880a40b9890a40f91f00086b08309f1a20d968f8c881009008bd43f908fddf08"
    "68020036400200b4080040f9c181009021b843f9080940f900013fd6800100b408008012088403b9c8810090086940f9"
    "080140f9a96987521f692938296987521f6929782800805208200e39f35341a9f55b42a9fd7bcda8c0035fd61f2003d5"
    "1f2003d51f2003d51f2003d51f2003d5"
    + "".join(n.encode().ljust(32, b"\0").hex() for n in ITEMS))

# (main offset, bytes)
PATCHES = [(MARKER_AT, MARKER), (HOOK, HOOK_BYTES), (SITE, SITE_BYTES)]


# Mailbox CALL_REQUEST: call main+FN(x0, x1) once, on the game's thread (for experiments). FN and the
# arguments are read from message-formatting scratch Ints (never saved).
CALL_REQUEST = TAG << 16 | 0x7F
CALL_FN, CALL_X0_LO, CALL_X0_HI, CALL_X1_LO, CALL_X1_HI = 0xC66E58F2, 0x54E43D73, 0x6206D0F1, 0x4D22AE4A, 0x10BCEEAC


def call_function(game, fn, x0=0, x1=0):
    """Have the patched game call main+fn(x0, x1) on its next frame."""
    for h, v in ((CALL_FN, fn), (CALL_X0_LO, x0 & 0xFFFFFFFF), (CALL_X0_HI, x0 >> 32),
                 (CALL_X1_LO, x1 & 0xFFFFFFFF), (CALL_X1_HI, x1 >> 32)):
        game.set_field(h, v - (1 << 32) if v >= 1 << 31 else v)
    game.set_field(MAILBOX, CALL_REQUEST)


def request_item(game, item, equip_direct=False, remove=False):
    """Ask the patched game to give (or with remove=True take away) `item` (an ITEMS name) through its own
    inventory code. equip_direct=False is how the Saria scene gives the Fairy Ocarina."""
    game.set_field(MAILBOX, TAG << 16 | int(bool(remove)) << 9 | int(bool(equip_direct)) << 8
                   | (ITEMS.index(item) + 1))


EQUIP_SLOT_FN = 0x2DF6C80  # main+: (equipment component, slot) -> create Link's gear for that slot
EQUIP_ALL_FN = 0xF201B0    # main+: (object holding the component at +0x470, 0) -> re-apply all gear (takes off empty slots)
EQUIP_SLOT = {"Item_Ocarina_Fairy": 0xD, "Item_Ocarina_Time": 0xD}  # slot 0xd reads PlayerStatus.EquipOcarina


def wait_handled(game, timeout=10.0):
    """Wait until the game has taken the pending request (it only runs while unpaused)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if game.get_field(MAILBOX) == 0:
            return True
        time.sleep(0.05)
    return False


def give_gear(game, item, log=lambda m: None, remove=False):
    """Give (or take away) gear the way the game does, so Link really wears / holds it. Blocks; run it
    on a thread (the game must be running to take each step).
    give:   PlayerStatus.Equip* = item; add through the game's inventory code; create it on Link
            (ocarina: main+0x2df6c80(component, 0xd); other gear: the full re-apply main+0xf201b0);
            redraw the HUD.
    remove: empty its PlayerStatus.Equip* if it's the one equipped; the game's remove code; the full
            re-apply (takes off gear whose slot is empty; the single-slot call doesn't); redraw the
            HUD; clear IsGet.<item>."""
    from gamedata import murmur3
    what = "remove" if remove else "give"
    slot = EQUIP_STR[item]
    if not remove:
        game.set_str(slot, item)
    elif game.get_str(slot) == item:
        game.set_str(slot, "")
    request_item(game, item, remove=remove)
    if not wait_handled(game):
        log(f"{what} {item}: the game didn't take the request")
        return False
    comp = game.equip_component()
    if not comp:
        log(f"{what} {item}: Link's equipment component not found")
        return False
    if remove or item not in EQUIP_SLOT:
        owner = game.equip_owner()
        if not owner:
            log(f"{what} {item}: the owner of Link's equipment component not found")
            return False
        call_function(game, EQUIP_ALL_FN, owner, 0)
    else:
        call_function(game, EQUIP_SLOT_FN, comp, EQUIP_SLOT[item])
    ok = wait_handled(game)
    if remove and ok:
        game.set_bool(murmur3("IsGet." + item), False)
    screen = ok and hud_screen(game)
    if screen:
        call_function(game, HUD_REFRESH_FN, screen)
        ok = wait_handled(game)
    log(f"{what} {item}: pouch request done; "
        f"{'equip call' if item in EQUIP_SLOT and not remove else 'gear re-apply'} done (component {comp:#x}); "
        f"hud refresh {'done' if screen and ok else 'failed'}")
    return ok


UI_MANAGER_PTR, DPAD_VTABLE = 0x4C7E098, 0x4BE1068  # PlayerStatus screen holds the D-pad widget at +0x330
# The PlayerStatus screen's virtual method 98 (+0x310): rebuilds the HUD; turns the D-pad on
# ([screen+0x330]) when it's enabled. Pausing/unpausing calls it (main+0xd212c0); caught with
# tools/dpadwatch.py. Takes only the screen.
HUD_REFRESH_FN = 0xA77FF0


# The four item-button slots: Enum fields (names not recovered) whose choices are the assignable items
# ("Empty", "DekuStick", ..., "SlingShot", ...). Found by their choice list; verified live: the slingshot the
# player had put on a button showed up in a0029837.
BUTTON_SLOTS = (0x6BEE602D, 0x76F85E6D, 0xA0029837, 0xBC32CE24)


def clear_buttons(game, choice, log=lambda m: None, still_removed=lambda: True, watch=120.0):
    """Take `choice` (an assignable-item name, e.g. "SlingShot") off any item button, then redraw the HUD.
    The in-game item wheel keeps its own copy of the buttons and writes it back when it closes, so keep
    watching for `watch` seconds and clear it again if it comes back while still_removed() (the item is
    still unticked). Blocks; run on a thread."""
    from gamedata import murmur3
    h, empty = murmur3(choice), murmur3("Empty")
    end = time.monotonic() + watch
    n = 0
    while time.monotonic() < end and still_removed():
        try:
            cleared = [s for s in BUTTON_SLOTS if game.get_typed("enum", s) == h]
            for s in cleared:
                game.set_typed("enum", s, empty)
            if cleared:
                n += 1
                screen = hud_screen(game)
                if screen and active(game):
                    call_function(game, HUD_REFRESH_FN, screen)
                    wait_handled(game)
                log(f"{choice}: taken off {len(cleared)} item button(s)" + (" again" if n > 1 else ""))
        except (OSError, KeyError, TypeError):
            pass  # game data moving (loading); try again
        time.sleep(0.3)
    return n > 0


def _chain(prefix, sizes=("Small", "Medium", "Large")):
    return [f"{prefix}_{s}" for s in sizes]


# Upgrades: menu setting -> (the items involved, {menu value: the items to hold}). The game's screens and
# story checks look at these pouch items, not just the capacity number the menu also writes.
UPGRADES = {
    "Deku Stick capacity": (_chain("Item_StickBag"), {10: ["Item_StickBag_Small"], 20: ["Item_StickBag_Medium"],
                                                      30: ["Item_StickBag_Large"]}),
    "Deku Nut capacity": (_chain("Item_NutBag"), {20: ["Item_NutBag_Small"], 30: ["Item_NutBag_Medium"],
                                                  40: ["Item_NutBag_Large"]}),
    "Deku Seed capacity": (_chain("Item_BulletBag"), {30: ["Item_BulletBag_Small"], 40: ["Item_BulletBag_Medium"],
                                                      50: ["Item_BulletBag_Large"]}),
    "Bomb capacity": (_chain("Item_BombBag"), {20: ["Item_BombBag_Small"], 30: ["Item_BombBag_Medium"],
                                               40: ["Item_BombBag_Large"]}),
    "Arrow capacity": (_chain("Item_Quiver"), {30: ["Item_Quiver_Small"], 40: ["Item_Quiver_Medium"],
                                               50: ["Item_Quiver_Large"]}),
    "Wallet capacity": (_chain("Item_Wallet"), {200: ["Item_Wallet_Small"], 500: ["Item_Wallet_Medium"],
                                                1000: ["Item_Wallet_Large"]}),
    "Magic capacity": (["Item_FairyBless_MP", "Item_FairyBless_MPDouble"],
                       {240: ["Item_FairyBless_MP"], 480: ["Item_FairyBless_MP", "Item_FairyBless_MPDouble"]}),
    "Double defense": (["Item_FairyBless_DefenseDouble"], {True: ["Item_FairyBless_DefenseDouble"]}),
    "Strength": (["Armor_Child_Gauntlet_Goron", "Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold"],
                 {"goron's bracelet": ["Armor_Child_Gauntlet_Goron"],
                  "silver gauntlets": ["Armor_Adult_Gauntlet_Silver"],
                  "golden gauntlets": ["Armor_Adult_Gauntlet_Gold"]}),
    "Scale": (["Item_Scale_Silver", "Item_Scale_Gold"],
              {"silver scale": ["Item_Scale_Silver"], "golden scale": ["Item_Scale_Gold"]}),
}


def set_upgrade(game, setting, value, log=lambda m: None):
    """Make the pouch hold what menu `setting` = `value` means (UPGRADES), through the game's own
    inventory code: remove the setting's other items, add the missing ones, redraw the HUD.
    Blocks; run on a thread."""
    from gamedata import murmur3
    chain, by_value = UPGRADES[setting]
    want = by_value.get(value, [])
    stock = lambda n: game.get_field(murmur3(f"PouchData.{n}.StockNum")) or 0  # noqa: E731
    done = []
    for n in chain:
        if n not in want and stock(n) > 0:
            request_item(game, n, remove=True)
            if not wait_handled(game):
                log(f"{setting}: the game didn't take the removal of {n}")
                return False
            game.set_bool(murmur3(f"IsGet.{n}"), False)
            done.append(f"-{n}")
    for n in want:
        if stock(n) == 0:
            request_item(game, n)
            if not wait_handled(game):
                log(f"{setting}: the game didn't take {n}")
                return False
            done.append(f"+{n}")
    screen = hud_screen(game)
    if done and screen:
        call_function(game, HUD_REFRESH_FN, screen)
        wait_handled(game)
    log(f"{setting} = {value}: {' '.join(done) or 'nothing to change'}; now "
        + ", ".join(f"{n}={stock(n)}" for n in chain))
    return True


def give_here(game, item, log=lambda m: None):
    """Give `item` through the game's own inventory code, as a chest does (dungeon items go to the dungeon
    Link is in), then redraw the HUD. Blocks; run on a thread."""
    request_item(game, item)
    ok = wait_handled(game)
    screen = ok and hud_screen(game)
    if screen:
        call_function(game, HUD_REFRESH_FN, screen)
        wait_handled(game)
    log(f"give {item} here ({game.get_str('map')}): {'done' if ok else 'the game did not take it'}")
    return ok


def unequip(game, slot, item, log=lambda m: None):
    """If Link has `item` equipped in PlayerStatus slot `slot` (a game.STR_IDX key), take it off: empty the
    slot, have the game re-apply all of Link's gear (main+0xf201b0, which takes off empty slots) and redraw
    the HUD. PlayerStatus.Equip* follow what the player equips in the pause menu, live. Blocks; run on a thread."""
    if game.get_str(slot) != item:
        return False
    game.set_str(slot, "")
    owner = game.equip_owner()
    if not owner:
        log(f"unequip {item}: the owner of Link's equipment component not found")
        return False
    call_function(game, EQUIP_ALL_FN, owner, 0)
    ok = wait_handled(game)
    screen = ok and hud_screen(game)
    if screen:
        call_function(game, HUD_REFRESH_FN, screen)
        ok = wait_handled(game)
    log(f"unequip {item}: slot {slot} emptied; gear re-apply {'done' if ok else 'not taken'}")
    return ok


MAP_SCREEN_VTABLE = 0x4BE8300  # the pause menu's map screen; its method 98 = main+0x33e12c0 re-reads the dungeon flags


def refresh_screen(game, vtable_off, log=lambda m: None):
    """Have the game call virtual method 98 (+0x310, the UI's "refresh contents", what opening/unpausing runs)
    on the UI screen whose vtable is main+vtable_off. Blocks; run on a thread."""
    q = game.gq
    uim = q(q(game.M + UI_MANAGER_PTR) or 0)
    if not uim:
        return False
    cnt = struct.unpack("<i", game.p.read(game.gb + uim + 8, 4))[0]
    arr = q(uim + 0x10)
    for i in range(min(cnt, 256)):
        s = q(arr + 8 * i)
        vt = q(s) if s else None
        if vt == game.M + vtable_off:
            fn = q(vt + 0x310) - game.M
            call_function(game, fn, s)
            ok = wait_handled(game)
            log(f"screen main+{vtable_off:#x}: refresh main+{fn:#x} {'done' if ok else 'not taken'}")
            return ok
    return False


def hud_screen(game):
    """Guest address of the PlayerStatus (HUD) screen, or None."""
    q = game.gq
    uim = q(q(game.M + UI_MANAGER_PTR) or 0)
    if not uim:
        return None
    cnt = struct.unpack("<i", game.p.read(game.gb + uim + 8, 4))[0]
    arr = q(uim + 0x10)
    for i in range(min(cnt, 256)):
        s = q(arr + 8 * i)
        w = q(s + 0x330) if s else None
        if w and q(w) == game.M + DPAD_VTABLE:
            return s
    return None


def patch_path():
    return (Path(os.path.expandvars(r"%APPDATA%")) / "eden" / "load" / GAME_ID / "ootmenu" / "exefs"
            / f"{BUILD_ID}.ips")


def build_ips(patches=PATCHES):
    out = bytearray(b"IPS32")
    for off, data in patches:
        assert CAVE_START <= off and off + len(data) <= CAVE_END or off < CAVE_START, "patch outside code"
        for i in range(0, len(data), 0xFFFF):
            chunk = data[i:i + 0xFFFF]
            out += struct.pack(">IH", off + i + NSO_HEADER, len(chunk)) + chunk
    out += b"EEOF"
    return bytes(out)


def install():
    """Write the patch file if missing or different. Returns 'installed', 'updated' or 'ok'."""
    p = patch_path()
    want = build_ips()
    if p.exists():
        if p.read_bytes() == want:
            return "ok"
        state = "updated"
    else:
        state = "installed"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(want)
    return state


def active(game):
    """True if the running game has exactly the current patch (None if unknown)."""
    if not (game and game.gb is not None and game.M is not None):
        return None
    try:
        return all(game.p.read(game.gb + game.M + off, len(data)) == data for off, data in PATCHES)
    except OSError:
        return None
