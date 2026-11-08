"""ootmenu - a gz-style practice menu overlay for Lichii (Ocarina of Time, Switch 2).

Runs as a separate program that sits on top of the emulator window. It never
takes keyboard focus and clicks pass through it, so the emulator keeps all
input. It reads the controller itself to drive the menu.

  python ootmenu.pyw          normal use, attaches to lichii.exe
  python ootmenu.pyw --test   standalone window for trying the menu without the emulator
"""

import json
import os
import re
import sys
import tempfile
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from pathlib import Path

import game
import patch
import guestmem
import win32
from menu import Choice, Info, Item, Menu, Number, Submenu, Toggle

EMULATOR_EXE = "lichii.exe"
VERSION = "0.1"
SETTINGS_PATH = Path(__file__).with_name("settings.json")

DEFAULTS = {
    # gz-style controller binds. Pad names: A B X Y LB RB LT RT START BACK
    # DUP DDOWN DLEFT DRIGHT LSTICK RSTICK, and C-UP/C-DOWN/C-LEFT/C-RIGHT
    # for the right stick (standing in for the N64 C-buttons).
    "open_combo": ["RB", "C-DOWN"],
    "close_combo": ["RB", "DLEFT"],
    "select": ["LB"],
    # Keyboard equivalents, only read while the emulator has focus.
    "open_key": "F3",
    "pause_while_open": True,  # sends the emulator's pause key (F4) on open/close
    "pause_key": "F4",
    "position": "top-left",
    "font_size": 16,
    "opacity": 90,
    "watches": [],
    # Put Link into the nearest exit when a warp is chosen, so it starts at once.
    "warp_auto_exit": True,
    # Play each spawn's own arrival cutscene instead of the standard arrival.
    "warp_arrival_cutscenes": False,
    # Hold to float upward, like gz's levitate (L on N64; LB is the ocarina here,
    # and the right stick moves the camera, so L3 it is).
    "levitate_combo": ["LSTICK"],
    "levitate_speed": 12,  # units per second
    "levitate_no_fall_damage": True,  # give back health lost landing from a levitate
    # On-screen watches (gz: R + D-Right toggles them).
    "watches_shown": True,
    "watches_combo": ["RB", "DRIGHT"],
    # gz timer/lag commands. Pad names are Xbox ones: Xbox B is the Switch A button.
    "timer_toggle_combo": ["RB", "B", "DLEFT"],   # gz: R + A + D-Left
    "timer_reset_combo": ["RB", "A", "DLEFT"],    # gz: R + B + D-Left
    "lag_reset_combo": ["RB", "A", "DRIGHT"],     # gz: R + B + D-Right
    "watches_position": "bottom-left",
    "hud_watches": ["x", "y", "z", "facing", "speed", "camera_angle"],
}

COLORS = {
    "bg": "#0c0f13", "border": "#2b3440", "title": "#ffffff", "text": "#d6dde6",
    "cursor": "#ffd75e", "dim": "#5f6b78", "status": "#8fd3ff",
}

REPEAT_DELAY, REPEAT_RATE = 0.40, 0.07  # seconds, like holding the d-pad in gz

LOG_PATH = Path(tempfile.gettempdir()) / "ootmenu.log"
WARPS_PATH = Path(__file__).with_name("warps.json")
MAX_ROWS = 16  # menu items shown at once; longer menus scroll
STANDARD_ARRIVAL = "Dm_SY_BancChangeArrive"

# Save fields (title -> MurmurHash3 of the field name). Mapped by exporting the
# onif.info/ocarina editor's demo save with changes and diffing the result.
FIELDS_PATH = Path(__file__).with_name("field_hashes.json")


ACQUIRED, ASSIGNMENT = "Acquisition history", "Assignment order"


def load_fields(group=None):
    """title -> hash. group=None gives the Int fields (counts, owned items, gear);
    group=ACQUIRED gives the Bool 'has been acquired' flags, which the game's item
    screen needs before it will show an item."""
    try:
        data = json.loads(FIELDS_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if group is None:
        return {m["title"]: int(h, 16) for h, m in data.items()
                if m.get("group") not in (ACQUIRED, ASSIGNMENT)}
    return {m["title"]: int(h, 16) for h, m in data.items() if m.get("group") == group}


# Acquired flags that go with a count or capacity (titles differ from the count's).
AMMO_ACQUIRED = {"Deku sticks": "Deku sticks", "Deku nuts": "Deku nuts", "Deku seeds": "Deku Seed",
                 "Bombs": "Bombs", "Arrows": "Arrows", "Bombchus": "Bombchus",
                 "Magic Bean": "Magic Bean"}
CAPACITY_ITEM = {
    "Deku Stick capacity": {10: "Stick Bag Small", 20: "Stick Bag Medium", 30: "Stick Bag Large"},
    "Deku Nut capacity": {20: "Nut Bag Small", 30: "Nut Bag Medium", 40: "Nut Bag Large"},
    "Deku Seed capacity": {30: "Bullet Bag Small", 40: "Bullet Bag Medium", 50: "Bullet Bag Large"},
    "Bomb capacity": {20: "Bomb Bag Small", 30: "Bomb Bag Medium", 40: "Bomb Bag Large"},
    "Arrow capacity": {30: "Quiver Small", 40: "Quiver Medium", 50: "Quiver Large"},
    "Wallet capacity": {500: "Wallet Medium", 1000: "Wallet Large"},
}


ITEMS = ["Fairy Slingshot", "Fairy Bow", "Arrow Fire", "Arrow Ice", "Arrow Light", "Boomerang",
         "Hookshot", "Longshot", "Hammer", "Lens of Truth", "Din’s Fire", "Farore’s Wind",
         "Nayru’s Love", "Gerudo membership card"]
GEAR = ["Kokiri Sword", "Master Sword", "Giant’s Knife", "Broken Giant’s Knife", "Biggoron’s Sword",
        "Deku Shield", "Hylian Shield", "Mirror Shield", "Goron Tunic", "Zora Tunic",
        "Iron Boots", "Hover Boots", "Fairy Ocarina", "Ocarina of Time"]
# One-off items given as plain pouch data: (menu label, pouch item, item-button choice name).
TRADE_ITEMS = [("weird egg", "Item_WeirdEgg", "WeirdEgg"), ("cucco", "Item_AlarmCucco", "AlarmCucco"),
               ("zelda's letter", "Item_ZeldaLetter", "ZeldaLetter"), ("pocket egg", "Item_PocketEgg", "PocketEgg"),
               ("pocket cucco", "Item_PocketCucco", "PocketCucco"), ("cojiro", "Item_CojiroCucco", "CojiroCucco"),
               ("odd mushroom", "Item_OddMushroom", "OddMushroom"), ("odd potion", "Item_OddPowder", "OddPowder"),
               ("poacher's saw", "Item_PoacherSaw", "PoacherSaw"), ("broken sword", "Item_BrokenSword", "BrokenSword"),
               ("prescription", "Item_Prescription", "Prescription"),
               ("eyeball frog", "Item_EyeballFrog", "EyeballFrog"), ("eye drops", "Item_EyeDrop", "EyeDrop"),
               ("claim check", "Item_ClaimCheck", "ClaimCheck")]
MASKS = [(f"{m.lower()} mask", f"Armor_Child_Mask_{m}", f"{m}Mask")
         for m in ("Keaton", "Skull", "Spooky", "Bunny", "Goron", "Zora", "Gerudo", "Truth")]
# inventory > items, in the order of the game's item screen (PouchTable ItemArray, Pa_ItemIcon_00..23). The
# trade items share one slot there; listed here in story order.
ITEM_SCREEN = ([("acquired", "Deku sticks"), ("owned", "Fairy Slingshot"), ("owned", "Boomerang"),
                ("acquired", "Deku nuts"), ("acquired", "Bombs"), ("acquired", "Bombchus"), ("owned", "Hammer"),
                ("owned", "Hookshot"), ("owned", "Longshot"), ("owned", "Fairy Bow"), ("owned", "Arrow Fire"),
                ("owned", "Arrow Ice"), ("owned", "Arrow Light"), ("acquired", "Magic Bean"),
                ("owned", "Din’s Fire"), ("owned", "Farore’s Wind"), ("owned", "Nayru’s Love")]
               + [("trade", "child"), ("trade", "adult")]
               + [("owned", "Lens of Truth"), ("owned", "Gerudo membership card"),
                  ("owned", "Fairy Ocarina"), ("owned", "Ocarina of Time")])
# (menu label, count field, capacity field or fixed maximum)
AMMO = [("deku sticks", "Deku sticks", "Deku Stick capacity"),
        ("deku nuts", "Deku nuts", "Deku Nut capacity"),
        ("deku seeds", "Deku seeds", "Deku Seed capacity"),
        ("bombs", "Bombs", "Bomb capacity"),
        ("arrows", "Arrows", "Arrow capacity"),
        ("bombchus", "Bombchus", 50),
        ("magic beans", "Magic Bean", 10)]
CAPACITIES = {"Deku Stick capacity": [0, 10, 20, 30], "Deku Nut capacity": [0, 20, 30, 40],
              "Deku Seed capacity": [0, 30, 40, 50], "Bomb capacity": [0, 20, 30, 40],
              # Magic: ~48 units per heart-width of bar; normal is 5 hearts long, double 10.
              # (The onif editor's 24/48 make a tiny bar.)
              "Arrow capacity": [0, 30, 40, 50], "Magic capacity": [0, 240, 480],
              "Wallet capacity": [0, 200, 500, 1000]}
# Fields mapped from the onif editor's demo save (tools: export with known changes,
# diff the save): songs, quest items, bottles, upgrades, dungeon items.
EXTRA_PATH = Path(__file__).with_name("extra_fields.json")


def load_extra():
    try:
        return json.loads(EXTRA_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


STORY_PATH = Path(__file__).with_name("story.json")


def load_story():
    """Quest stage enums and action-guide 'seen' flags, read from the game's own
    GameDataList and event scripts (see tools/byml.py, tools/evdump.py)."""
    try:
        return json.loads(STORY_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


QUEST_NAMES = {"Step_Get_StoneOfForest": "deku tree", "Step_Get_StoneOfFire": "dodongo's cavern",
               "Step_Get_StoneOfWater": "jabu-jabu", "Step_SageOfForest": "forest temple",
               "Step_SageOfFire": "fire temple", "Step_SageOfWater": "water temple",
               "Step_SageOfShadow": "shadow temple", "Step_SageOfSpirit": "spirit temple",
               "Step_EscapeFromCrumblingGanonCastle": "castle escape", "Step_GatheringTokenGold": "skulltulas",
               "Step_SubEvent_GetSwordBigGoron": "biggoron sword", "Step_SubEvent_AllBossBattle": "boss rush",
               "Step_Goto_HyruleCastle": "zelda / castle", "Step_Meet_DekuTree": "kokiri sword",
               "Step_Minigame_SkullKidSession": "skull kid song"}


def quest_label(name):
    """Step_Get_StoneOfForest -> 'stone of forest'."""
    if name in QUEST_NAMES:
        return QUEST_NAMES[name]
    n = re.sub(r"^Step_(Get_|Goto_|SubEvent_|Mini[Gg]ame_)?", "", name).replace("_", " ")
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", n).lower()


SONGS = ["Zelda's Lullaby", "Epona's Song", "Saria's Song", "Sun's Song", "Song of Time", "Song of Storms",
         "Scarecrow's Song", "Minuet of Forest", "Bolero of Fire", "Serenade of Water", "Requiem of Spirit",
         "Nocturne of Shadow", "Prelude of Light"]
QUEST_ITEMS = ["Kokiri's Emerald", "Goron's Ruby", "Zora's Sapphire", "Forest Medallion", "Fire Medallion",
               "Water Medallion", "Spirit Medallion", "Shadow Medallion", "Light Medallion"]
DUNGEON_LIST = [("Inside The Deku Tree", "deku tree"), ("Dodongo Cavern", "dodongo's cavern"),
            ("Inside Jabu Jabu Belly", "jabu-jabu"), ("Forest Temple", "forest temple"),
            ("Fire Temple", "fire temple"), ("Water Temple", "water temple"),
            ("Shadow Temple", "shadow temple"), ("Spirit Temple", "spirit temple"),
            ("Bottom Of The Well", "bottom of well"), ("Ice Cavern", "ice cavern"),
            ("Gerudo Training Ground", "gerudo training"), ("Gerudo Fortress", "gerudo fortress"),
            ("Inside Ganon Castle", "ganon's castle"), ("Final Battle Field", "final battle")]
DUNGEON_ITEMS = [("Map", "map", "Dungeon Map"), ("Compass", "compass", "Dungeon Compass"),
                 ("Boss key", "boss key", "Dungeon Boss Key")]
# Bottle contents -> its "acquired" flag title.
BOTTLE_ACQ = {"Red Potion": "Bottle Red Potion", "Green Potion": "Bottle Green Potion",
              "Blue Potion": "Bottle Blue Potion", "Blue Fire": "Bottle Blue Fire", "Fish": "Bottle Fish",
              "Letter": "Bottle Letter", "Big Poe": "Bottle Big Poe", "Poe": "Bottle Poe", "Bug": "Bottle Bug",
              "Fairy": "Bottle Fairy", "Max Milk": "Bottle Milk", "Half Milk": "Bottle Half Milk"}
STRENGTH = [("none", None), ("goron's bracelet", "Child Gauntlet Goron"),
            ("silver gauntlets", "Adult Gauntlet Silver"), ("golden gauntlets", "Adult Gauntlet Gold")]
SCALES = [("none", 3.0, None), ("silver scale", 6.0, "Scale Silver"), ("golden scale", 9.0, "Scale Gold")]

DUNGEON_KEYS = ["Forest Temple", "Fire Temple", "Water Temple", "Shadow Temple", "Spirit Temple",
                "Bottom Of The Well", "Gerudo Training Ground", "Inside Ganon Castle"]


def label_of(title):
    return title.lower().replace("’", "'")

# Watches that can be shown on screen (key, label), in display order. Besides
# these, custom memory watches (gz-style address + type) come from settings.json.
WATCHES = [
    ("x", "x"), ("y", "y"), ("z", "z"),
    ("facing", "facing"), ("move_angle", "move angle"),
    ("speed", "speed"), ("y_speed", "y speed"),
    ("camera_angle", "camera angle"), ("camera_pitch", "camera pitch"),
    ("camera_dist", "camera dist"),
    ("input", "input"), ("stick", "stick"),                       # gz input display
    ("lag", "lag"), ("timer", "timer"),                           # gz lag counter/timer
    ("frame", "frame"),
    ("map", "map"),
]
WATCH_LABELS = dict(WATCHES)

# XInput buttons shown with the Switch names Lichii maps them to (Xbox B = Switch A).
SWITCH_NAMES = [("B", "A"), ("A", "B"), ("Y", "X"), ("X", "Y"), ("LB", "L"), ("RB", "R"),
                ("LT", "ZL"), ("RT", "ZR"), ("START", "+"), ("BACK", "-"),
                ("LSTICK", "L3"), ("RSTICK", "R3"), ("DUP", "Du"), ("DDOWN", "Dd"),
                ("DLEFT", "Dl"), ("DRIGHT", "Dr")]


class FrameStats:
    """gz's lag counter, timer and pause display, driven by the game's frame counter.

    lag = 60 Hz ticks of real time minus game frames since the last reset, as in
    gz (which counts vertical interrupts against game frames)."""

    def __init__(self):
        self.count = None
        self.changed_at = time.monotonic()
        self.history = []  # (time, count) over the last second, for fps
        self.reset_lag()
        # Timer: counts while running and not held (held = menu open).
        self.timer_total, self.timer_running, self.timer_held, self.timer_since = 0.0, False, False, None

    def update(self, count):
        now = time.monotonic()
        if count is None:
            return
        if count != self.count:
            self.count, self.changed_at = count, now
        if self.lag_start is None:
            self.lag_start = (now, count)
        self.history = [h for h in self.history if now - h[0] < 1.0] + [(now, count)]

    def reset_lag(self):
        self.lag_start = None

    @property
    def lag(self):
        if not self.lag_start or self.count is None:
            return None
        t0, c0 = self.lag_start
        return int(round((time.monotonic() - t0) * 60 - (self.count - c0)))

    @property
    def fps(self):
        if len(self.history) < 2:
            return None
        (t0, c0), (t1, c1) = self.history[0], self.history[-1]
        return (c1 - c0) / (t1 - t0) if t1 > t0 else None

    @property
    def paused(self):
        return time.monotonic() - self.changed_at > 0.25

    def _timer_sync(self):
        """Bank elapsed time and (re)start or stop counting to match the state."""
        now = time.monotonic()
        if self.timer_since is not None:
            self.timer_total += now - self.timer_since
        self.timer_since = now if self.timer_running and not self.timer_held else None

    def timer_toggle(self):
        self.timer_running = not self.timer_running
        self._timer_sync()

    def timer_reset(self):
        self.timer_total = 0.0
        self._timer_sync()
        self.timer_total = 0.0

    def timer_hold(self, held):
        """Pause the timer and lag counter while the menu is open (the game is
        paused then, so real time would otherwise all count as lag)."""
        if held != self.timer_held:
            self.timer_held = held
            self._timer_sync()
            now = time.monotonic()
            if held:
                self._hold_from = (now, self.count)
            elif self.lag_start and getattr(self, "_hold_from", None):
                t_held, c_held = self._hold_from
                frames = (self.count or 0) - (c_held or 0)  # if the game ran anyway
                t0, c0 = self.lag_start
                self.lag_start = (t0 + (now - t_held), c0 + frames)

    @property
    def timer(self):
        t = self.timer_total + (time.monotonic() - self.timer_since if self.timer_since else 0)
        state = "" if self.timer_since else "  (stopped)" if not self.timer_running else "  (menu)"
        return f"{int(t // 60)}:{t % 60:06.3f}{state}"


def angle_text(deg):
    """Degrees plus gz-style 16-bit hex (0x0000-0xFFFF for a full turn)."""
    return f"{deg:5.1f}  {int(round(deg / 360 * 65536)) & 0xFFFF:#06x}"

# Spawns on the far side of something you have to bomb open. Until it's open in
# your save, Link arrives inside the rock and the arrival never finishes (black
# screen, no control; only a reload gets out).
GATED_SPAWNS = {
    "MountainTrail_DodongoCavernSide": "boulder bombed",
    "GoronCity_LostWoodSide_before": "lost woods wall bombed",
    "GoronCity_LostWoodSide_after": "lost woods wall bombed",
    "LostWood_GoronCitySide_before": "goron city wall bombed",
    "LostWood_GoronCitySide_after": "goron city wall bombed",
}

DUNGEONS = {"InsideTheDekuTree", "DodongoCavern", "InsideJabuJabuBelly", "ForestTemple",
            "FireTemple", "WaterTemple", "ShadowTemple", "SpiritTemple", "BottomOfTheWell",
            "IceCavern", "GerudoTrainingGround", "InsideGanonCastle", "CrumblingGanonCastle"}


def pretty(name):
    """'KokiriForest_Child' -> 'kokiri forest (child)'."""
    parts = name.split("_")
    tags = [p.lower() for p in parts[1:] if p in ("Child", "Adult", "Alternative")]
    words = " ".join(p for p in parts if p.lower() not in tags)
    words = re.sub(r"(?<=[a-z])(?=[A-Z0-9])|(?<=[A-Z])(?=[A-Z][a-z])", " ", words).lower()
    for typo, fixed in NAME_FIXES.items():  # typos in the game's internal names
        words = words.replace(typo, fixed)
    return words + (f" ({' '.join(tags)})" if tags else "")


NAME_FIXES = {"hounted": "haunted", "musuem": "museum", "caslte": "castle", "wateland": "wasteland",
              "caven": "cavern"}


def log(msg):
    try:
        with LOG_PATH.open("a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")
    except OSError:
        pass


def load_settings():
    s = dict(DEFAULTS)
    try:
        s.update(json.loads(SETTINGS_PATH.read_text()))
    except (OSError, ValueError):
        pass
    return s


# ====================================================================== input

class Input:
    """Turns raw pad/keyboard state into gz-style menu events each tick."""

    KEYS = ["UP", "DOWN", "LEFT", "RIGHT", "ENTER", "BACKSPACE"]

    def __init__(self, settings):
        self.s = settings
        self.prev = set()
        self.swallowed = set()  # buttons used by a combo, ignored until released
        self.repeat_at = {}

    def poll(self, keyboard):
        held = set(win32.read_pad())
        if keyboard:
            held |= {f"KEY:{k}" for k in self.KEYS + [self.s["open_key"]] if win32.key_down(k)}
        return held

    def update(self, keyboard):
        held = self.poll(keyboard)
        events, fired = [], []

        for name, combo in (("toggle", self.s["open_combo"]),
                            ("toggle", [f"KEY:{self.s['open_key']}"]),
                            ("close", self.s["close_combo"]),
                            ("timer_toggle", self.s["timer_toggle_combo"]),
                            ("timer_reset", self.s["timer_reset_combo"]),
                            ("lag_reset", self.s["lag_reset_combo"]),
                            ("watches", self.s["watches_combo"])):
            if combo and all(b in held for b in combo) and not all(b in self.prev for b in combo):
                fired.append((name, set(combo)))
        # Like gz, a press that completes several commands (R+D-Right and
        # R+B+D-Right) only runs the most specific one.
        for name, combo in fired:
            if not any(combo < other for _, other in fired):
                events.append(name)
                self.swallowed |= combo

        self.swallowed &= held
        live = held - self.swallowed
        # While a combo modifier (e.g. R) is held, the d-pad belongs to combos.
        modifiers = {self.s["open_combo"][0], self.s["close_combo"][0]}
        nav_ok = not (live & modifiers)

        now = time.monotonic()
        actions = {
            "up": ["DUP", "KEY:UP"], "down": ["DDOWN", "KEY:DOWN"],
            "left": ["DLEFT", "KEY:LEFT"], "right": ["DRIGHT", "KEY:RIGHT"],
        }
        for action, buttons in actions.items():
            if nav_ok and any(b in live for b in buttons):
                if action not in self.repeat_at:
                    events.append(action)
                    self.repeat_at[action] = now + REPEAT_DELAY
                elif now >= self.repeat_at[action]:
                    events.append(action)
                    self.repeat_at[action] = now + REPEAT_RATE
            else:
                self.repeat_at.pop(action, None)

        new = live - self.prev
        if nav_ok and new & set(self.s["select"] + ["KEY:ENTER"]):
            events.append("select")
        if new & {"KEY:BACKSPACE"}:
            events.append("back")

        self.prev = held
        return events


# ====================================================================== app

class App:
    def __init__(self, test=False):
        self.test = test
        self.settings = load_settings()
        self.input = Input(self.settings)
        self.is_open = test
        self.paused_by_us = False
        self.status_text, self.status_until = "", 0.0

        self.pid = self.hwnd = self.proc = self.ram = self.game = None
        self.cheats = {}
        self.warp_exits = {}
        self.frames = FrameStats()
        self.next_attach = 0.0
        self.toast_until = time.monotonic() + 6.0  # "I'm running" notice on start
        self.toast_custom = None
        # Code patch (patch.py): install/update now; nag until the running game has it.
        try:
            self.patch_state = patch.install()
        except OSError as e:
            self.patch_state = "error"
            log(f"patch: could not install: {e}")
        log(f"patch: {self.patch_state} (v{patch.VERSION})")
        self.patch_active = None
        self._patch_check = 0.0

        self.root_menu = self.build_menus()
        self.stack = [self.root_menu]

        win32.make_dpi_aware()
        self.tk = tk.Tk()
        self.tk.title("ootmenu")
        self.tk.configure(bg=COLORS["bg"])
        self.canvas = tk.Canvas(self.tk, bg=COLORS["bg"], highlightthickness=0, bd=0)
        self.canvas.pack(fill="both", expand=True)
        if not test:
            self.tk.overrideredirect(True)
            self.tk.attributes("-topmost", True)
        self.apply_style()
        self.tk.update_idletasks()
        self.hwnd_self = win32.root_hwnd(self.canvas.winfo_id())
        if not test:
            win32.make_overlay(self.hwnd_self)
            win32.show_no_activate(self.hwnd_self, False)
        self.shown = test

        # On-screen watches: a second click-through window, like gz's watch display.
        self.hud = self.hwnd_hud = None
        self.hud_shown, self.hud_last = False, None
        if not test:
            self.hud = tk.Toplevel(self.tk)
            self.hud.configure(bg=COLORS["bg"])
            self.hud.overrideredirect(True)
            self.hud.attributes("-topmost", True)
            self.hud_canvas = tk.Canvas(self.hud, bg=COLORS["bg"], highlightthickness=0, bd=0)
            self.hud_canvas.pack(fill="both", expand=True)
            self.apply_style()
            self.hud.update_idletasks()
            self.hwnd_hud = win32.root_hwnd(self.hud_canvas.winfo_id())
            win32.make_overlay(self.hwnd_hud)
            win32.show_no_activate(self.hwnd_hud, False)
        self.last_frame = None
        self.tick()

    # ------------------------------------------------------------ menus

    def build_menus(self):
        todo = lambda *labels: [Item(l) for l in labels]  # noqa: E731  unmapped stubs
        s = self.settings

        warps = Menu("warps", [
            Info(lambda: f"current   {self.game_str('map')}"),
            Info(lambda: f"warp      {self.warp_desc()}"),
            Submenu("places", self.build_places()),
            Item("cancel warp", handler=lambda app: app.cancel_warp()),
            Toggle("arrival cutscenes", lambda: s["warp_arrival_cutscenes"],
                   lambda v: s.__setitem__("warp_arrival_cutscenes", v)),
        ] + todo("age", "cutscene"))

        scene = Menu("scene", todo(
            "reload scene", "void out", "set entrance point", "clear flags",
            "noclip", "collision view"))

        def cheat(name):
            return Toggle(name, lambda: self.cheats.get(name, False),
                          lambda v: self.cheats.__setitem__(name, v))

        cheats = Menu("cheats", [cheat("energy"), cheat("rupees"), cheat("magic")]
                      + [cheat(label) for label, _, _ in AMMO] + [Toggle(l) for l in (
                          "small keys", "nayru's love", "freeze time", "no music", "items usable",
                          "isg", "quick text", "no hud")])

        g = lambda: self.game  # noqa: E731  (the Game object changes if lichii restarts)
        hearts = lambda q: f"{q / 4:g}"  # noqa: E731  quarter hearts -> hearts

        # --- inventory / equipment editing (gz's inventory and equips menus)
        self.gear_wanted = {}  # item -> requested state while its patch job is pending
        F = self.fields = load_fields()
        self.acquired = load_fields(ACQUIRED)

        def field(title):
            return lambda: g().get_field(F[title])

        def set_field(title):
            def set_(v):
                v = int(v)
                g().set_field(F[title], v)
                # The game only lists items whose "acquired" flag is set, so set
                # it too, as picking the item up normally would.
                if v > 0:
                    extra = (CAPACITY_ITEM.get(title, {}).get(v) or AMMO_ACQUIRED.get(title)
                             or (title if title in ITEMS + GEAR else None))
                    self.mark_acquired(extra)
            return set_

        # Given / taken away through the game's own code (patch.give_gear): the ocarina has no equip
        # step, so the game must create it on Link. Other gear stays plain data; the player equips it
        # in the pause menu, which runs the game's own equip code.
        GAME_GEAR = {"Fairy Ocarina": "Item_Ocarina_Fairy", "Ocarina of Time": "Item_Ocarina_Time"}

        def owned(title):
            def set_(v):
                if title in GAME_GEAR and not self.patch_active:
                    self.status(f"{label_of(title)} needs the ootmenu patch: restart the game", 4)
                    return
                if title in GAME_GEAR:
                    item = GAME_GEAR[title]
                    if v and item.startswith("Item_Ocarina"):
                        # The D-pad (and so the ocarina's slot on it) only appears once the Navi
                        # tutorial has been seen, normally on entering the Deku Tree.
                        navi = load_story().get("action_guides", {}).get("Navi")
                        if navi:
                            g().set_bool(int(navi, 16), True)
                    # On the game's thread once it runs: add (or remove) it through the game's own
                    # inventory code, put it on (or take it off) Link, redraw the HUD.
                    self.gear_wanted[item] = bool(v)
                    patch.submit(("gear", item), patch.give_gear, g(), item, log, remove=not v)
                    self.status(f"{label_of(title)}: {'given' if v else 'removed'} when the game runs "
                                f"(close the menu)", 4)
                    return
                set_field(title)(1 if v else 0)
                if not v and title in WORN_GEAR and self.patch_active:
                    # Unticked while Link is wearing it: take it off too (patch.unequip).
                    slot, item = WORN_GEAR[title]
                    patch.submit(("unequip", item), patch.unequip, g(), slot, item, log)
                if not v and title in BUTTON_ITEM:
                    # Unticked while it's on an item button: take it off the button too.
                    # Watches for a while: the item wheel writes its copy back when it closes.
                    threading.Thread(target=patch.clear_buttons, args=(g(), BUTTON_ITEM[title], log),
                                     kwargs={"still_removed": lambda: not self.safe(field(title))},
                                     daemon=True).start()
            def get():
                item = GAME_GEAR.get(title)
                if item and patch.pending(("gear", item)):
                    return self.gear_wanted.get(item)  # what was asked for; the game applies it once it runs
                return bool(self.safe(field(title)))
            return Toggle(label_of(title), get, set_)

        # Items the player can put on an item button: menu title -> the button slots' choice name.
        BUTTON_ITEM = {"Fairy Slingshot": "SlingShot", "Fairy Bow": "Bow", "Arrow Fire": "FireArrow",
                       "Arrow Ice": "IceArrow", "Arrow Light": "LightArrow", "Boomerang": "Boomerang",
                       "Hookshot": "Hookshot", "Longshot": "Longshot", "Hammer": "Hammer",
                       "Lens of Truth": "GlassTruth", "Din’s Fire": "DinFire",
                       "Farore’s Wind": "FaroreWind", "Nayru’s Love": "NayruLove",
                       "Iron Boots": "HeavyBoot", "Hover Boots": "HoverBoot"}

        # Gear the player equips in the pause menu: menu title -> (PlayerStatus slot, game item name).
        WORN_GEAR = {"Kokiri Sword": ("sword", "Weapon_Sword_Kokiri"),
                     "Master Sword": ("sword", "Weapon_Sword_Master"),
                     "Giant’s Knife": ("sword", "Weapon_Sword_Giant"),
                     "Broken Giant’s Knife": ("sword", "Weapon_Sword_BrokenGiant"),
                     "Biggoron’s Sword": ("sword", "Weapon_Sword_Biggoron"),
                     "Deku Shield": ("shield", "Weapon_Shield_Deku"),
                     "Hylian Shield": ("shield", "Weapon_Shield_Hylian"),
                     "Mirror Shield": ("shield", "Weapon_Shield_Mirror"),
                     "Goron Tunic": ("tunic", "Armor_Adult_Goron"), "Zora Tunic": ("tunic", "Armor_Adult_Zora"),
                     "Iron Boots": ("boots", "Armor_Adult_HeavyBoot"),
                     "Hover Boots": ("boots", "Armor_Adult_HoverBoot")}

        def pouch_toggle(label, item, choice):
            """One-off items (trade items, masks): ticked = count 1 + IsGet.<item>, which is what the game's
            item screen lists. Unticked also takes it off the item buttons (patch.clear_buttons)."""
            from gamedata import murmur3
            stock, isget = murmur3(f"PouchData.{item}.StockNum"), murmur3("IsGet." + item)

            def set_(v):
                g().set_field(stock, 1 if v else 0)
                g().set_bool(isget, bool(v))
                if not v:
                    threading.Thread(target=patch.clear_buttons, args=(g(), choice, log),
                                     kwargs={"still_removed": lambda: not self.safe(lambda: g().get_field(stock))},
                                     daemon=True).start()
            return Toggle(label, lambda: bool(self.safe(lambda: g().get_field(stock))), set_)

        def trade_choice(age):
            """The trade-item slot: one item at a time, scrolled with left/right. Choosing one takes the previous
            one away (count, acquired flag) and gives the new one; an item button holding the old one switches to
            the new one, as trading does in the game ("none" empties it)."""
            from gamedata import murmur3
            group = TRADE_ITEMS[:3] if age == "child" else TRADE_ITEMS[3:]  # child: egg, cucco, letter
            names = ["none"] + [lbl for lbl, _, _ in group]
            by_label = {lbl: (item, choice) for lbl, item, choice in group}

            def held():
                for lbl, item, _ in group:
                    if g().get_field(murmur3(f"PouchData.{item}.StockNum")):
                        return lbl
                return "none"

            def get():
                return self.safe(held, "-")

            def set_(lbl):
                old = [(l, i, c) for l, i, c in group
                       if g().get_field(murmur3(f"PouchData.{i}.StockNum")) and l != lbl]
                for _, item, _ in old:
                    g().set_field(murmur3(f"PouchData.{item}.StockNum"), 0)
                    g().set_bool(murmur3("IsGet." + item), False)
                new = by_label.get(lbl)
                if new:
                    g().set_field(murmur3(f"PouchData.{new[0]}.StockNum"), 1)
                    g().set_bool(murmur3("IsGet." + new[0]), True)
                # Buttons holding an old trade item now hold the new one (or nothing).
                olds = {murmur3(c) for _, _, c in old}
                target = murmur3(new[1] if new else "Empty")
                changed = False
                for s in patch.BUTTON_SLOTS:
                    if g().get_typed("enum", s) in olds:
                        g().set_typed("enum", s, target)
                        changed = True
                if changed and self.patch_active:
                    patch.submit(("refresh", patch.HUD_VTABLE), patch.refresh_screen, g(), patch.HUD_VTABLE, log)
            return Choice(f"{age} trade", names, get, set_)

        def acquired_toggle(title):
            # Sticks and nuts have no "owned" field, only counts; the acquired
            # flag is what puts them on the item screen.
            h = self.acquired.get(title)
            return Toggle(label_of(title), lambda: bool(self.safe(lambda: g().get_bool(h))),
                          lambda v: g().set_bool(h, v))

        def limit(cap):
            return (lambda: cap) if isinstance(cap, int) else field(cap)

        def fill(title, cap):
            def go(app):
                if title == "Current magic":
                    g().set_magic(int(limit(cap)()))
                else:
                    set_field(title)(limit(cap)())
                app.status(f"{label_of(title)} filled")
            return go

        def upgrade(setting, value):
            """Make the pouch hold the item(s) for this upgrade setting, through the game's own code
            (patch.set_upgrade). The game's screens and story checks look at those, not the numbers."""
            if self.patch_active:
                patch.submit(("upgrade", setting), patch.set_upgrade, g(), setting, value, log)

        def capacity(title, label):
            def set_(v):
                set_field(title)(v)
                if title in patch.UPGRADES:
                    upgrade(title, int(v))
            def get():
                # The upgrade item Link holds (what the game shows); the number if he holds none.
                if title in patch.UPGRADES and self.game and self.game.ready:
                    v = self.safe(lambda: patch.held_upgrade(g(), title))
                    if v is not None:
                        return v
                    if title != "Deku Stick capacity":  # sticks hold 10 without any bag item
                        return 0  # no bag / quiver / magic item: the game shows none
                return self.safe(field(title), 0)
            return Choice(label, CAPACITIES[title], get, set_)

        ammo = Menu("ammo", [Number(label, field(t), set_field(t), hi=limit(cap),
                                    on_select=fill(t, cap)) for label, t, cap in AMMO]
                    + [Info(lambda: "d-left/right change, L fills")])
        caps = Menu("capacity", [capacity(t, label_of(t).replace(" capacity", ""))
                                 for t in CAPACITIES if t not in ("Magic capacity", "Wallet capacity")]
                    + [capacity("Wallet capacity", "wallet")])
        magic = Menu("magic", [
            Number("magic", field("Current magic"), lambda v: g().set_magic(int(v)),
                   hi=field("Magic capacity"), on_select=fill("Current magic", "Magic capacity")),
            capacity("Magic capacity", "magic meter"),
            Info(lambda: "meter: 0 none, 240 normal, 480 double"),
            Info(lambda: "(set the meter first, then magic)"),
        ])
        amounts = Menu("amounts", [
            Number("skulltulas", field("Gold Skulltula tokens"), set_field("Gold Skulltula tokens"),
                   hi=lambda: 100),
        ])
        keys = Menu("small keys", [Number(label_of(d), field(f"{d} small keys"),
                                          set_field(f"{d} small keys"), hi=lambda: 9)
                                   for d in DUNGEON_KEYS])
        # --- songs, quest items, bottles, upgrades, dungeon items (extra_fields.json)
        X = self.extra = load_extra()

        def xh(key):
            return int(X[key]["hash"], 16)

        def flag(label, keys, acq=None, setting=None, refresh=None):
            """Toggle over one or more fields (Bool, or Int 0/1), all set together."""
            def get():
                if setting:
                    held = self.safe(lambda: patch.held_upgrade(g(), setting))
                    if held is not None:
                        return bool(held)
                k = keys[0]
                fn = (g().get_bool if X[k]["type"] == "Bool" else g().get_field)
                return bool(self.safe(lambda: fn(xh(k))))

            def set_(v):
                for k in keys:
                    if X[k]["type"] == "Bool":
                        g().set_bool(xh(k), v)
                    else:
                        g().set_field(xh(k), 1 if v else 0)
                if setting and self.patch_active:
                    upgrade(setting, bool(v))
                elif v:
                    self.mark_acquired(acq)
                if refresh and self.patch_active:
                    # Redraw that screen the way opening it does (e.g. the map screen re-reads the flags).
                    patch.submit(("refresh", refresh), patch.refresh_screen, g(), refresh, log)
            return Toggle(label, get, set_)

        def bottle(letter):
            own, avail = f"inventory/Items/Bottle {letter}", f"inventory/Bottles/Bottle {letter} available"
            c = X[f"inventory/Bottles/Bottle {letter} contents"]
            ch = [(v, lbl.lower(), lbl) for v, lbl in c["choices"]]

            def get():
                def read():
                    if not g().get_field(xh(own)):
                        return "none"
                    v = g().get_typed("enum", int(c["hash"], 16))
                    return next((lo for val, lo, _ in ch if val == v), "?")
                return self.safe(read, "-")

            def set_(o):
                g().set_field(xh(own), 0 if o == "none" else 1)
                g().set_bool(xh(avail), o != "none")
                if o != "none":
                    val, _, lbl = next(x for x in ch if x[1] == o)
                    g().set_typed("enum", int(c["hash"], 16), val)
                    self.mark_acquired("Bottle")
                    self.mark_acquired(BOTTLE_ACQ.get(lbl))
            return Choice(f"bottle {letter.lower()}", ["none"] + [lo for _, lo, _ in ch], get, set_)

        def strength():
            k = "link/Abilities/Strength"
            vals = [v for v, _ in X[k]["choices"]]  # None, Low, Middle, High

            def get():
                held = self.safe(lambda: patch.held_upgrade(g(), "Strength"))
                if held is not None:
                    return held
                v = self.safe(lambda: g().get_typed("enum", xh(k)))
                return STRENGTH[vals.index(v)][0] if v in vals else "-"

            def set_(o):
                i = [n for n, _ in STRENGTH].index(o)
                g().set_typed("enum", xh(k), vals[i])
                if self.patch_active:
                    upgrade("Strength", o)
                else:
                    self.mark_acquired(STRENGTH[i][1])
            return Choice("strength", [n for n, _ in STRENGTH], get, set_)

        def scale():
            k = "link/Abilities/Diving depth"

            def get():
                held = self.safe(lambda: patch.held_upgrade(g(), "Scale"))
                if held is not None:
                    return held
                v = self.safe(lambda: g().get_typed("float", xh(k)))
                return next((n for n, d, _ in SCALES if v is not None and abs(v - d) < 0.01), "-")

            def set_(o):
                n, d, acq = next(s for s in SCALES if s[0] == o)
                g().set_typed("float", xh(k), d)
                if self.patch_active:
                    upgrade("Scale", n)
                else:
                    self.mark_acquired(acq)
            return Choice("scale", [n for n, _, _ in SCALES], get, set_)

        if X:
            bottles = Menu("bottles", [bottle(c) for c in "ABCDE"]
                           + [Info(lambda: "l / d-right: next contents")])
            upgrades = Menu("upgrades", [
                strength(), scale(),
                flag("double defense", ["link/Abilities/Double defense"], setting="Double defense"),
                flag("stone of agony", ["equipment/Effects & appearance/Stone of Agony equipped"],
                     "Agony Stone"),
            ])
            quest = Menu("quest items", [flag(label_of(q), [f"quests/Quest items/{q}", f"inventory/Items/{q}"])
                                         for q in QUEST_ITEMS])
            songs = Menu("songs", [flag(label_of(s), [f"quests/Songs/{s}"]) for s in SONGS])
            dungeon_items = Menu("dungeon items", [
                Submenu(label, Menu(label, [flag(lbl, [f"dungeons/{d}/{t}"], acq, refresh=patch.MAP_SCREEN_VTABLE)
                                            for t, lbl, acq in DUNGEON_ITEMS]))
                for d, label in DUNGEON_LIST])
            extra = [Submenu("bottles", bottles), Submenu("upgrades", upgrades),
                     Submenu("quest items", quest), Submenu("songs", songs),
                     Submenu("dungeon items", dungeon_items)]
        else:
            extra = [Info(lambda: "extra_fields.json missing")]

        if F:
            inventory = Menu("inventory", [
                # Same order as the game's item screen (romfs UI/Pouch/PouchTable ItemArray), then what this game
                # shows on other screens (lens: equipment, card: collection), then the ocarinas.
                Submenu("items", Menu("items", [
                    acquired_toggle(t) if kind == "acquired" else owned(t) if kind == "owned" else trade_choice(t)
                    for kind, t in ITEM_SCREEN])),
                Submenu("ammo", ammo), Submenu("capacity", caps), Submenu("magic", magic),
                *extra,
                Submenu("small keys", keys), Submenu("amounts", amounts),
                # The L/R quick menus only open with at least two items acquired
                # (there's nothing to switch between with one).
                Info(lambda: "L/R quick menus need 2+ items"),
            ])
        else:
            inventory = Menu("inventory", [Info(lambda: "field_hashes.json missing")])

        def fill_health(app):
            g().set_health(g().health()[1])
            app.status("health refilled")

        def fill_rupees(app):
            g().set_int("rupees", g().get_int("wallet"))
            app.status("wallet filled")

        link = Menu("link", [
            Info(lambda: f"map           {self.game_str('map')}"),
            Number("health", lambda: g().health()[0], lambda v: g().set_health(v),
                   hi=lambda: g().health()[1], fmt=hearts, on_select=fill_health),
            Info(lambda: f"max health    {self.game_val(lambda: hearts(g().health()[1]))}"),
            Number("rupees", lambda: g().get_int("rupees"), lambda v: g().set_int("rupees", v),
                   hi=lambda: g().get_int("wallet"), on_select=fill_rupees),
            Info(lambda: f"wallet        {self.game_val(lambda: g().get_int('wallet'))}"),
            Info(lambda: f"position      {self.pos_desc()}"),
            Info(lambda: "d-left/right change, L fills"),
        ])

        equips = Menu("equips", [Info(lambda k=k: f"{k:<9} {self.game_str(k) or 'none'}")
                                 for k in ("sword", "shield", "tunic", "boots", "arrow", "ocarina")]
                      + ([Submenu("owned gear", Menu("owned gear", [owned(t) for t in GEAR] + [pouch_toggle(*m) for m in MASKS]))]
                         if F else []))
        file = Menu("file", todo("time of day", "age", "set flags", "restore skulltulas",
                                 "save", "load"))
        # Built-in watches, plus custom ones from settings.json "watches":
        # [{"name", "offset" (hex RAM offset), "size"}]
        def set_on_hud(key, on):
            chosen = set(s["hud_watches"]) | {key} if on else set(s["hud_watches"]) - {key}
            s["hud_watches"] = [k for k, _ in WATCHES if k in chosen]  # keep a fixed order

        def on_hud(key):
            return Toggle(WATCH_LABELS[key], lambda: key in s["hud_watches"],
                          lambda v: set_on_hud(key, v), detail=lambda: self.watch_text(key))

        watches = Menu("watches", [
            Toggle("show on screen", lambda: s["watches_shown"],
                   lambda v: s.__setitem__("watches_shown", v)),
            Choice("screen corner", ["top-left", "top-right", "bottom-left", "bottom-right"],
                   lambda: s["watches_position"], lambda v: s.__setitem__("watches_position", v)),
            Info(lambda: "L adds/removes a watch; R + D-right shows/hides"),
            Item("start/stop timer", handler=lambda app: app.frames.timer_toggle()),
            Item("reset timer", handler=lambda app: app.frames.timer_reset()),
            Item("reset lag counter", handler=lambda app: app.frames.reset_lag()),
        ] + [on_hud(k) for k, _ in WATCHES]
          + [Info(lambda w=w: f"    {w.get('name', '?'):<13} {self.custom_watch_text(w)}")
             for w in s["watches"]])

        emulator = Menu("emulator", [
            Info(lambda: f"process   {self.emulator_status()}"),
            Info(lambda: f"window    {self.window_desc()}"),
            Info(lambda: f"memory    {self.memory_desc()}"),
            Info(lambda: f"guest ram {self.ram_desc()}"),
            Info(lambda: f"game      {self.game.state if self.game else '-'}"),
            Item("pause / resume", handler=lambda app: app.tap_emulator_key(s["pause_key"])),
        ])

        def setter(key):
            def set_(v):
                s[key] = v
            return set_

        def restore(app):
            s.clear()
            s.update(DEFAULTS)
            app.on_setting_changed()
            app.status("settings restored")

        settings = Menu("settings", [
            Choice("position", ["top-left", "top-right", "bottom-left", "bottom-right"],
                   lambda: s["position"], setter("position")),
            Choice("font size", list(range(12, 32, 2)), lambda: s["font_size"], setter("font_size")),
            Choice("opacity", [50, 60, 70, 80, 90, 100], lambda: s["opacity"], setter("opacity")),
            Toggle("pause while open", lambda: s["pause_while_open"],
                   lambda v: s.__setitem__("pause_while_open", v)),
            Toggle("warp auto-exit", lambda: s["warp_auto_exit"],
                   lambda v: s.__setitem__("warp_auto_exit", v)),
            Info(lambda: f"open      {' + '.join(s['open_combo'])}  /  {s['open_key']}"),
            Info(lambda: f"close     {' + '.join(s['close_combo'])}"),
            Info(lambda: f"select    {' / '.join(s['select'])}  /  Enter"),
            Info(lambda: f"levitate  hold {' + '.join(s['levitate_combo']) or 'off'}"),
            Choice("levitate speed", [4, 8, 12, 20, 30], lambda: s["levitate_speed"],
                   setter("levitate_speed")),
            Toggle("no levitate fall damage", lambda: s["levitate_no_fall_damage"],
                   lambda v: s.__setitem__("levitate_no_fall_damage", v)),
            Item("restore defaults", handler=restore),
        ])

        about = Menu("about", [
            Info(lambda: f"ootmenu {VERSION}"),
            Info(lambda: "gz-style menu overlay for lichii"),
            Info(lambda: "dim entries are not mapped yet"),
        ])

        # --- story: each quest is one stage enum (value = MurmurHash3 of the stage name)
        from gamedata import murmur3
        ST = load_story()

        def quest(h, q):
            stages = q["stages"]
            by_hash = {murmur3(n): n for n in stages}

            def get():
                v = self.safe(lambda: g().get_typed("enum", int(h, 16)))
                return by_hash.get(v, "-").lower()

            def set_(o):
                name = next(n for n in stages if n.lower() == o)
                g().set_typed("enum", int(h, 16), murmur3(name))
            return Choice(quest_label(q["name"]), [n.lower() for n in stages], get, set_)

        def tutorials_done(app):
            n = 0
            for name, h in ST.get("action_guides", {}).items():
                try:
                    g().set_bool(int(h, 16), True)
                    n += 1
                except (OSError, RuntimeError, KeyError):
                    pass
            app.status(f"{n} tutorials marked seen")

        if ST:
            quests = sorted(ST["quests"].items(), key=lambda x: quest_label(x[1]["name"]))
            story = Menu("story", [Item("all tutorials seen", tutorials_done)]
                         + [quest(h, q) for h, q in quests]
                         + [Info(lambda: "l / d-right: next stage"),
                            Info(lambda: "load zone to apply")])
        else:
            story = Menu("story", [Info(lambda: "story.json missing")])

        return Menu(f"ootmenu {VERSION}", [
            Submenu("link", link),
            Submenu("warps", warps), Submenu("scene", scene), Submenu("cheats", cheats),
            Submenu("inventory", inventory), Submenu("equips", equips), Submenu("story", story),
            Submenu("file", file),
            Submenu("watches", watches), Submenu("emulator", emulator),
            Submenu("settings", settings), Submenu("about", about),
        ], root=True)

    # ------------------------------------------------------------ warps

    def build_places(self):
        """places > category > map > spawn point, from warps.json."""
        try:
            data = json.loads(WARPS_PATH.read_text())
            spawns, self.warp_exits = data["spawns"], data.get("exits", {})
        except (OSError, ValueError, KeyError):
            return Menu("places", [Info(lambda: "warps.json missing")])

        def category(name):
            if name in DUNGEONS:
                return "dungeons"
            if name.endswith("Stage") or name in ("BossRushIntervalRoom", "FinalBattleField"):
                return "boss & battle rooms"
            if name.endswith("_Child"):
                return "overworld (child)"
            if "_Adult" in name or name in ("HyruleCastle", "GanonCastle"):
                return "overworld (adult)"
            return "other"

        def spawn_label(pos, source):
            if pos in GATED_SPAWNS:
                return f"{pretty(pos)}  [needs {GATED_SPAWNS[pos]}]"
            label = pretty(pos)
            # The source map's age tag comes from adult-only data, so leave it out.
            origin = pretty(source[5:]).split(" (")[0] if source.startswith("from ") else ""
            if origin and origin != label:
                return f"{label}  (from {origin})"
            return label

        groups = {}
        for name in sorted(spawns, key=pretty):
            items = [Item(spawn_label(pos, src),
                          handler=lambda app, n=name, p=pos, e=event: app.start_warp(n, p, e))
                     for pos, event, src in spawns[name]]
            groups.setdefault(category(name), []).append(Submenu(pretty(name), Menu(pretty(name), items)))
        order = ["overworld (child)", "overworld (adult)", "dungeons", "boss & battle rooms", "other"]
        return Menu("places", [Submenu(c, Menu(c, groups[c])) for c in order if c in groups])

    def start_warp(self, map_name, spawn, event):
        if not (self.game and self.game.ready):
            self.status(f"can't warp: {self.game.state if self.game else 'lichii not running'}")
            return
        g = self.game
        # Loading zones of the current map: Link is put into the nearest one so
        # the warp starts straight away.
        exits = (self.warp_exits.get(g.get_str("map"))
                 if self.settings["warp_auto_exit"] else None)
        # Spawn-specific arrival cutscenes (walking out of a particular door, story
        # 'before'/'after' variants) can hang when the game state doesn't match,
        # leaving Link stuck with no control. The standard arrival just places him.
        if not self.settings["warp_arrival_cutscenes"]:
            event = STANDARD_ARRIVAL
        try:
            g.arm_warp(map_name, spawn, event)
        except (OSError, RuntimeError) as e:
            self.status(f"warp failed: {e}")
            return
        log(f"warp armed: {map_name} / {spawn}; exits={len(exits or [])}")
        self.set_open(False)  # unpauses; moving Link needs the game running
        if exits:
            threading.Thread(target=self._enter_exit, args=(g, g.warp, exits), daemon=True).start()
            self.notify([(f"warping to {pretty(map_name)}...", COLORS["title"])], 4)
        else:
            self.walk_notice(map_name, "no exit data for this map")

    def _enter_exit(self, g, w, exits):
        """Background: once the game has resumed, put Link into the nearest exit."""
        time.sleep(0.3)
        try:
            w["entered"] = g.enter_nearest_exit(exits)
        except (OSError, RuntimeError) as e:
            log(f"warp: couldn't move Link: {e}")
            w["entered"] = False

    def walk_notice(self, map_name, reason):
        self.notify([(f"warp ready: {pretty(map_name)}", COLORS["title"]),
                     ("walk through any exit or loading zone", COLORS["status"]),
                     (f"({reason})", COLORS["dim"])], 8)

    def cancel_warp(self):
        if self.game and self.game.warp and self.game.warp["state"] == "armed":
            self.game.cancel_warp()
            self.status("warp cancelled")
        else:
            self.status("no warp armed")

    def pos_desc(self):
        g = self.game
        if not (g and g.ready):
            return "-"
        d = g.display_pos()
        return "%.1f, %.1f, %.1f" % d if d else "-"

    def warp_desc(self):
        w = self.game.warp if self.game else None
        if not w:
            return "none"
        return f"{w['state']}: {pretty(w['map'])}"

    def watch_warp(self):
        """Tell the player when an armed warp fires or expires."""
        w = self.game.warp if self.game else None
        if w and w["state"] == "armed" and w.get("entered") is False and not w.get("walk_told"):
            w["walk_told"] = True
            self.walk_notice(w["map"], "couldn't move Link into an exit")
        if w and w["state"] != "armed" and not w.get("reported"):
            w["reported"] = True
            if w["state"] == "done":
                self.notify([(f"warping to {pretty(w['map'])}", COLORS["title"])], 3)
            elif w["state"] != "cancelled":
                self.notify([(f"warp {w['state']}", COLORS["status"])], 4)

    # ------------------------------------------------------------ navigation

    def enter(self, menu):
        menu.cursor = 0
        self.stack.append(menu)

    def go_back(self):
        if len(self.stack) > 1:
            self.stack.pop()
        else:
            self.set_open(False)

    def set_open(self, open_):
        if open_ == self.is_open:
            return
        self.is_open = open_
        log(f"menu {'open' if open_ else 'closed'} focused={self.emulator_focused()} "
            f"hwnd={self.hwnd and hex(self.hwnd)}")
        if self.test:
            return
        if open_ and self.settings["pause_while_open"] and self.emulator_focused():
            win32.tap_key(self.settings["pause_key"])
            self.paused_by_us = True
        elif not open_ and self.paused_by_us:
            if self.emulator_focused():
                win32.tap_key(self.settings["pause_key"])
            self.paused_by_us = False

    def handle(self, event):
        menu = self.stack[-1]
        if event == "toggle":
            self.set_open(not self.is_open)
        elif event == "watches":
            self.settings["watches_shown"] = not self.settings["watches_shown"]
            self.on_setting_changed()
        elif event == "timer_toggle":
            self.frames.timer_toggle()
        elif event == "timer_reset":
            self.frames.timer_reset()
        elif event == "lag_reset":
            self.frames.reset_lag()
        elif not self.is_open:
            return
        elif event == "close":
            self.set_open(False)
        elif event == "up":
            menu.move(-1)
        elif event == "down":
            menu.move(1)
        elif event in ("left", "right"):
            menu.current.adjust(self, -1 if event == "left" else 1)
        elif event == "select":
            menu.current.select(self)
        elif event == "back":
            self.go_back()

    def status(self, text, seconds=2.5):
        self.status_text, self.status_until = text, time.monotonic() + seconds

    def on_setting_changed(self):
        self.apply_style()
        try:
            SETTINGS_PATH.write_text(json.dumps(self.settings, indent=2))
        except OSError as e:
            self.status(f"could not save settings: {e}")

    def tap_emulator_key(self, key):
        if self.test:
            self.status(f"(test mode) would press {key}")
        elif self.emulator_focused():
            win32.tap_key(key)
            # our own pause state is now unknown, so don't unpause on close
            self.paused_by_us = False
        else:
            self.status("emulator is not focused")

    # ------------------------------------------------------------ emulator

    def refresh_emulator(self):
        now = time.monotonic()
        if now < self.next_attach:
            return
        self.next_attach = now + 1.0
        pid = win32.find_pid(EMULATOR_EXE)
        if pid != self.pid:
            if self.game:
                self.game.close()
            if self.proc:
                self.proc.close()
            self.pid, self.proc, self.ram, self.game = pid, None, None, None
            if pid:
                try:
                    self.proc = win32.Process(pid)
                    self.game = game.Game(self.proc)
                    self.game.log = log
                except OSError:
                    self.proc = None
        if self.proc and not self.ram:  # appears once a game is booted
            try:
                self.ram = guestmem.GuestRam(self.proc)
                log(f"guest ram at {self.ram.base:#x}, {self.ram.size / 2**30:.2f} GiB")
            except (LookupError, OSError):
                self.ram = None
        self.hwnd = win32.main_window_of(self.pid) if self.pid else None

    def emulator_focused(self):
        if not self.pid:
            return False
        fg = win32.foreground_window()
        owner = win32.wt.DWORD()
        win32.user32.GetWindowThreadProcessId(fg, win32.ctypes.byref(owner))
        return owner.value == self.pid

    def emulator_status(self):
        if not self.pid:
            return f"{EMULATOR_EXE} not running"
        return f"pid {self.pid}" + ("" if self.proc else " (no memory access)")

    def window_desc(self):
        if not self.hwnd:
            return "-"
        _, _, w, h = win32.client_rect_on_screen(self.hwnd)
        return f"{w} x {h}"

    def game_val(self, fn):
        """fn() if the game data is available, else '-'."""
        if not (self.game and self.game.ready):
            return "-"
        try:
            v = fn()
        except Exception:
            return "-"
        return "-" if v is None else v

    def mark_acquired(self, title):
        h = self.acquired.get(title) if title else None
        if h is None:
            return
        try:
            self.game.set_bool(h, True)
        except (OSError, RuntimeError, KeyError) as e:
            log(f"couldn't mark {title} acquired: {e}")

    def safe(self, fn, default=None):
        """fn() if the game data is available and readable, else default."""
        if not (self.game and self.game.ready):
            return default
        try:
            v = fn()
        except (OSError, KeyError, TypeError):
            return default
        return default if v is None else v

    def game_str(self, key):
        return self.game_val(lambda: self.game.get_str(key))

    def apply_cheats(self):
        g = self.game
        if not (g and g.ready):
            return
        try:
            if self.cheats.get("energy"):
                hp = g.health()
                if hp and hp[0] < hp[1]:
                    g.set_health(hp[1])
            if self.cheats.get("rupees"):
                wallet, rupees = g.get_int("wallet"), g.get_int("rupees")
                if wallet is not None and rupees != wallet:
                    g.set_int("rupees", wallet)
            # Magic and ammo: keep each at its capacity (gz-style "max" cheats).
            F = self.fields
            targets = [("magic", "Current magic", "Magic capacity")] + AMMO
            for name, title, cap in targets:
                if self.cheats.get(name) and title in F:
                    top = cap if isinstance(cap, int) else g.get_field(F[cap])
                    if top is not None and g.get_field(F[title]) != top:
                        if title == "Current magic":
                            g.set_magic(top)
                        else:
                            g.set_field(F[title], top)
        except (OSError, KeyError):
            pass  # mid scene load; try again next tick

    def apply_levitate(self, focused):
        """Hold the levitate button(s) to rise; not while the menu is open."""
        g, held = self.game, self.input.prev
        combo = self.settings["levitate_combo"]
        active = (g and g.ready and focused and not self.is_open and combo
                  and all(b in held for b in combo))
        if g:
            g.protect_landing = self.settings["levitate_no_fall_damage"]
        if active:
            g.levitate_start(self.settings["levitate_speed"])
        elif g:
            g.levitate_stop()

    def ram_desc(self):
        return f"{self.ram.size / 2**30:.0f} GiB found" if self.ram else "not found"

    def watch_desc(self, w):
        name = f"{w['name']:<12}"
        if not self.ram:
            return f"{name}-"
        try:
            return f"{name}{self.ram.read_int(int(w['offset'], 16), w.get('size', 4), signed=True)}"
        except (OSError, ValueError) as e:
            return f"{name}<{e}>"

    def memory_desc(self):
        return f"{self.proc.working_set_bytes() / 2**30:.2f} GiB" if self.proc else "-"

    # ------------------------------------------------------------ drawing

    def apply_style(self):
        self.font = tkfont.Font(family="Consolas", size=-self.settings["font_size"])
        self.hud_font = tkfont.Font(family="Consolas", size=-max(10, self.settings["font_size"] - 2))
        self.tk.attributes("-alpha", self.settings["opacity"] / 100)
        if getattr(self, "hud", None):
            self.hud.attributes("-alpha", self.settings["opacity"] / 100)
            self.hud_last = None
        self.last_frame = None

    def frame_lines(self):
        """[(text, color)] for the current state."""
        menu = self.stack[-1]
        path = " / ".join(m.title for m in self.stack[1:]) or "main"
        lines = [(self.stack[0].title, COLORS["title"]), (path, COLORS["dim"]), ("", None)]
        # Scroll long menus so the cursor stays in view.
        n = len(menu.items)
        top = 0 if n <= MAX_ROWS else max(0, min(menu.cursor - MAX_ROWS // 2, n - MAX_ROWS))
        bottom = min(n, top + MAX_ROWS)
        lines.append((f"  ^ {top} more" if top else "", COLORS["dim"]))
        for i in range(top, bottom):
            item = menu.items[i]
            mark = ">" if i == menu.cursor else " "
            color = (COLORS["cursor"] if i == menu.cursor
                     else COLORS["text"] if item.mapped else COLORS["dim"])
            lines.append((f"{mark} {item.text()}", color))
        if n > MAX_ROWS:
            lines.append((f"  v {n - bottom} more" if bottom < n else "", COLORS["dim"]))
        lines.append(("", None))
        if time.monotonic() < self.status_until:
            lines.append((self.status_text, COLORS["status"]))
        else:
            lines.append(("d-pad move  L select  R+D-left close", COLORS["dim"]))
        return lines

    def draw(self, lines):
        return self.draw_on(self.canvas, self.font, lines)

    def draw_on(self, canvas, font, lines):
        pad = round(font.metrics("linespace") * 0.6)
        lh = font.metrics("linespace")
        width = max(font.measure(t) for t, _ in lines) + pad * 2
        height = lh * len(lines) + pad * 2
        canvas.delete("all")
        canvas.config(width=width, height=height)
        canvas.create_rectangle(0, 0, width - 1, height - 1, outline=COLORS["border"])
        for i, (text, color) in enumerate(lines):
            if text:
                canvas.create_text(pad, pad + i * lh, text=text, fill=color,
                                   font=font, anchor="nw")
        return width, height

    # ------------------------------------------------------------ watches

    def watch_values(self):
        """Live values for every watch, read at most once per frame."""
        now = time.monotonic()
        if now - getattr(self, "_watch_t", 0) < 0.012:
            return self._watch_vals
        vals = {}
        g = self.game
        if g and g.ready:
            d = g.display_pos()
            if d:
                vals.update(x=d[0], y=d[1], z=d[2])
            vals.update(g.link_motion())
            vals["map"] = g.get_str("map")
            f = self.frames
            vals.update(frame=f.count, fps=f.fps, lag=f.lag, timer=f.timer,
                        paused="PAUSED" if f.paused else "no")
        held = self.input.prev
        vals["input"] = " ".join(sw for xb, sw in SWITCH_NAMES if xb in held) or "."
        lx, ly = win32.last_sticks[:2]
        vals["stick"] = f"{round(lx / 327.67):+4d} {round(ly / 327.67):+4d}"  # -100..100
        self._watch_t, self._watch_vals = now, vals
        return vals

    def watch_text(self, key):
        v = self.watch_values().get(key)
        if v is None or v == "":
            return "-"
        if key in ("facing", "move_angle", "camera_angle"):
            return angle_text(v)
        if key == "camera_pitch":
            return f"{v:+6.1f} deg"
        if key == "fps":
            return f"{v:5.1f}"
        if isinstance(v, float):
            return f"{v:8.2f}"
        return str(v)

    def custom_watch_text(self, w):
        """A settings.json watch: {"name", "address" (game address, hex), "type"} as in
        gz (u8 s8 x8 u16 s16 x16 u32 s32 x32 f32, plus f64), or the older
        {"name", "offset" (RAM offset), "size"}."""
        if "address" not in w:
            return self.watch_desc(w).split(None, 1)[-1]
        if not (self.game and self.game.ready):
            return "-"
        try:
            return str(self.game.read_typed(int(str(w["address"]), 16), w.get("type", "u32")))
        except (OSError, ValueError, KeyError) as e:
            return f"<{e}>"

    def update_hud(self, game_shown):
        if not self.hud:
            return
        s = self.settings
        visible = bool(s["watches_shown"] and s["hud_watches"] and game_shown
                       and self.game and self.game.ready)
        if visible:
            lines = [(f"{WATCH_LABELS[k]:<12} {self.watch_text(k)}", COLORS["text"])
                     for k in s["hud_watches"] if k in WATCH_LABELS]
            lines += [(f"{w.get('name', '?'):<12} {self.custom_watch_text(w)}", COLORS["text"])
                      for w in s["watches"] if w.get("show", True)]
            if not lines:
                lines = [("no watches selected", COLORS["dim"])]
            area = win32.client_rect_on_screen(self.hwnd)
            frame = (tuple(lines), area, s["watches_position"], s["font_size"])
            if frame != self.hud_last:
                w, h = self.draw_on(self.hud_canvas, self.hud_font, lines)
                x, y, aw, ah = area
                pos = s["watches_position"]
                px = x + (aw - w - 24 if pos.endswith("right") else 24)
                py = y + (ah - h - 48 if pos.startswith("bottom") else 56)
                self.hud.update_idletasks()
                win32.move_topmost(self.hwnd_hud, px, py, w, h)
                self.hud_last = frame
        if visible != self.hud_shown:
            win32.show_no_activate(self.hwnd_hud, visible)
            self.hud_shown = visible

    def notify(self, lines, seconds):
        """Show a short notice over the game while the menu is closed."""
        self.toast_custom = lines
        self.toast_until = time.monotonic() + seconds

    def toast_lines(self):
        if self.toast_custom:
            return self.toast_custom
        combo = " + ".join(self.settings["open_combo"])
        waiting = "" if self.pid else "  (waiting for lichii.exe)"
        return [(f"ootmenu {VERSION} running{waiting}", COLORS["title"]),
                (f"open in game: {combo}  or  {self.settings['open_key']}", COLORS["text"]),
                ("run run.bat again to stop it", COLORS["dim"])]

    def place(self, width, height, area):
        if self.test:
            self.tk.geometry(f"{width}x{height}")
            return
        x, y, w, h = area
        mx, my_top, my_bottom = 24, 56, 48  # clear the emulator's menu/status bars
        pos = self.settings["position"]
        px = x + (w - width - mx if pos.endswith("right") else mx)
        py = y + (h - height - my_bottom if pos.startswith("bottom") else my_top)
        # Tk's own geometry() ignores the position for this kind of window (and
        # isn't multi-monitor/DPI safe), so let Windows place it directly.
        self.tk.update_idletasks()
        win32.move_topmost(self.hwnd_self, px, py, width, height)

    # ------------------------------------------------------------ main loop

    def watch_patch(self):
        """Keep a notice on screen until the running game has the current code patch."""
        now = time.monotonic()
        if now >= self._patch_check:
            self._patch_check = now + 1.0
            self.patch_active = patch.active(self.game) if (self.game and self.game.gb) else None
        if self.patch_active is False and not self.is_open:
            first = {"updated": "ootmenu patch updated"}.get(self.patch_state, "ootmenu patch installed")
            self.notify([(first, COLORS["title"]), ("please restart the game", COLORS["text"])], 1.5)

    def tick(self):
        try:
            self.refresh_emulator()
            focused = self.emulator_focused()
            for event in self.input.update(keyboard=self.test or focused):
                self.handle(event)
            self.apply_cheats()
            self.watch_patch()
            self.watch_warp()
            self.apply_levitate(focused)
            if self.game and self.game.ready:
                self.frames.update(self.game.frame_count())
            self.frames.timer_hold(self.is_open)

            game_shown = bool(focused and self.hwnd and not win32.is_minimized(self.hwnd))
            menu_visible = self.is_open and (self.test or game_shown)
            toast = (not self.test and not menu_visible
                     and time.monotonic() < self.toast_until)
            visible = menu_visible or toast
            if visible:
                lines = self.frame_lines() if menu_visible else self.toast_lines()
                area = (win32.client_rect_on_screen(self.hwnd) if game_shown
                        else win32.screen_work_area())
                frame = (tuple(lines), area, tuple(self.settings.items()))
                if frame != self.last_frame:
                    self.place(*self.draw(lines), area)
                    self.last_frame = frame
            if visible != self.shown and not self.test:
                win32.show_no_activate(self.hwnd_self, visible)
                self.shown = visible
            self.update_hud(game_shown)
        except Exception as e:  # keep the overlay alive, but leave a trace
            log(f"error: {e!r}")
        finally:
            self.tk.after(16, self.tick)


PID_PATH = Path(tempfile.gettempdir()) / "ootmenu.pid"


def main():
    test = "--test" in sys.argv
    if not test:
        if not win32.claim_single_instance("Local\\ootmenu-lichii"):
            # Already running: offer to stop it (the overlay has no window to close).
            if win32.ask_yes_no("ootmenu", "ootmenu is already running.\n\nStop it?"):
                try:
                    win32.kill_process(int(PID_PATH.read_text()))
                except (OSError, ValueError):
                    pass
            return
        PID_PATH.write_text(str(os.getpid()))
    App(test=test).tk.mainloop()


if __name__ == "__main__":
    main()
