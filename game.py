"""Live game state for Ocarina of Time (Switch 2, v131072) running in Lichii.

Guest (Switch) addresses map linearly into the emulator: host = guestbase + guest.
`Game` finds guestbase and the game's main module, then:

  - Link's health, through Link's actor (method and offsets from the oot26-tools
    Cheat Engine table: github.com/mrissaoussama/oot26-tools).
  - GameData, the live save fields. Each field is keyed by MurmurHash3 of its name.
    Int fields are objects 0x28 apart, value at +0x1C (and a copy at +0x20); a
    hash table of (hash: u32, index: u32) pairs sits right before them.
  - GameData strings (current map etc.), also found as in oot26-tools.

Discovery runs on a background thread; reads and writes are cheap and can be
done from the UI thread.
"""

import ctypes
import math
import re
import struct
import threading
import time

import guestmem
import win32

# --- main-module offsets (oot26-tools, game v131072) ----------------------
DATA_OFF = 0x484D000
CHECK_OFF, CHECK_VAL_OFF = 0x4C7EFE0, 0x4DD71A0
VT_LINKOBJ_OFF, VT_PARAM_OFF = 0x4B25E18, 0x4B63438
LINKOBJ_PTRS = [(0x4E371D0, 0x480), (0x4E94858, 0x2E0), (0x4E935B8, 0xEF8),
                (0x4E94860, 0x328), (0x4E94368, 0xD70)]
DISP_OFF = 0x4C7E740
CAMERA_PTR_OFF = 0x4E98068  # [[main+this]+0x68]: game camera; +0x38 position, +0x44 look-at
# Link object fields (oot26-tools): facing direction (cos at +0x328, sin at
# +0x330, 0 = +Z) and velocity (+0x388 x, +0x38C y, +0x390 z), units/s.
LINK_FACING_COS, LINK_FACING_SIN, LINK_VELOCITY = 0x328, 0x330, 0x388
VT_INTFIELD_OFF = 0x4C20718  # vtable of a GameData Int field object
VT_BOOLFIELD_OFF = 0x4C20658  # vtable of a GameData Bool field object
VT_BOOLTYPE_OFF = 0x4C1F140   # vtable of the Bool entry in GameData's type list

# --- GameData ---------------------------------------------------------------
GD_RANGE = (0x2180000000, 0x2190000000)  # guest range holding GameData
INT_STRIDE, INT_VALUE = 0x28, 0x1C
STR_ANCHOR = bytes.fromhex("DA89611D1F000000")  # Sequence_CurrentBanc, index 0x1F
STR_FIRST_VALUE, STR_STRIDE = 0x207C, 0x108
STR_IDX = {"map": 0x1F, "event": 0x20, "starter": 0x21, "next_map": 0x22, "spawn": 0x23,
           "sword": 0x17, "shield": 0x16, "tunic": 0x10, "boots": 0x12,
           "arrow": 0x11, "ocarina": 0x15}
STR_LEN = 64

WARP_TIMEOUT = 300.0  # seconds to wait for Link to take an exit

# Link's position: doubles (x, y, z) in a physics record (see move_link).
POS_RANGES = [(0x2160000000, 0x2168000000),   # where it has always been so far (~0.3 s)
              (0x2100000000, 0x2300000000), (0x2000000000, 0x4000000000)]
POS_NEAR = 64 << 20  # first search this far around the last record that worked
PHYS_Y = 0.585  # Link's physics record sits this far above the drawn position
FRAME_COUNTER_OFF = 0x4E84BA0  # main+: u32, +1 per game update
# Soft landing after levitate: a fall from rest (units/s^2, units/s, s).
LAND_G, LAND_MAX_SPEED, LAND_TIMEOUT = 40.0, 60.0, 5.0
# Motion state captured from Link mid-run; written after the position so the
# game stops treating him as standing still and applies the move at once.
WAKE = bytes.fromhex("000000002567154100000000cecf8d40fbf99f3e263ca1bf"
                     "000000002467154100000000cccf8d40fcf99f3e243ca1bf")

# Int fields identified so far (hash of the field name).
INT_HASH = {
    "rupees": 0xA77921D7,
    "hearts": 0x31AB5580,      # quarter hearts, as stored in the save
    "max_hearts": 0xFBE01DA1,  # quarter hearts
    "wallet": 0xE0010A50,
}

# Magic meter object: holds the real current magic. Every frame it writes
# clamp(value, 0, Magic capacity) into the Current magic save field, so writing
# only the save field gets undone. Found by the two field hashes it keeps at +0x38
# (found with a write watchpoint, main+0x163a40; see tools/magicwatch.py).
MAGIC_HASH, MAGIC_CAP_HASH = 0x76B18200, 0x12D63A97
MAGIC_KEYS, MAGIC_VALUE = 0x38, 0x44
MAGIC_RANGE = (0x2100000000, 0x2300000000)
MAGIC_RETRY = 5.0  # seconds between searches while it's missing

MEM_COMMIT = 0x1000
READABLE = (0x02, 0x04, 0x20, 0x40)
WRITABLE = (0x04, 0x40)


class Game:
    def __init__(self, proc):
        self.p = proc
        self.gb = self.M = None          # guestbase (host), main module (guest)
        self.hp = self.hp_max = None     # guest addresses of Link's health ints
        self.int_base = None             # guest address of Int field object 0
        self.int_idx = {}                # name -> index
        self.str_base = None             # guest address of String64 field 0
        self.state = "searching"
        self.pos = None                  # Link's physics record the last time a move worked
        self.link_obj = self.camera = None  # refreshed every half second
        self._missing_since = None       # when the save data was last found missing
        self.log = lambda msg: None      # the app replaces this with its logger
        self._stop = False
        threading.Thread(target=self._loop, daemon=True).start()

    def close(self):
        self._stop = True

    # ------------------------------------------------------------ raw access
    def _q(self, host):
        try:
            return struct.unpack("<Q", self.p.read(host, 8))[0]
        except OSError:
            return None

    def gq(self, g):
        return None if (g is None or self.gb is None) else self._q(self.gb + g)

    def read_i32(self, g):
        return struct.unpack("<i", self.p.read(self.gb + g, 4))[0]

    def write_i32(self, g, v):
        self.p.write(self.gb + g, struct.pack("<i", int(v)))

    # ------------------------------------------------------------ discovery
    def _valid_base(self, gb, M):
        if not gb or not M:
            return False
        return self._q(gb + M + CHECK_OFF) == M + CHECK_VAL_OFF

    def _find_base(self):
        k, m, addr = guestmem.k, guestmem.MBI(), 0
        while k.VirtualQueryEx(self.p.handle, addr, ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == MEM_COMMIT and m.RegionSize > CHECK_OFF - DATA_OFF:
                v = self._q(m.BaseAddress + CHECK_OFF - DATA_OFF)
                if v and v > CHECK_VAL_OFF and (v - CHECK_VAL_OFF) % 0x1000 == 0:
                    M = v - CHECK_VAL_OFF
                    gb = m.BaseAddress - (M + DATA_OFF)
                    if self._valid_base(gb, M):
                        return gb, M
            addr = (m.BaseAddress or 0) + m.RegionSize
            if addr >= 1 << 47:
                break
        return None, None

    def _link_obj(self):
        for a, b in LINKOBJ_PTRS:
            st = self.gq(self.M + a)
            o = self.gq(st + b) if st else None
            if o and self.gq(o) == self.M + VT_LINKOBJ_OFF:
                return o
        return None

    # Link's equipment component: the object the game passes to main+0x2df6c80(component, slot)
    # to create Link's gear from PlayerStatus.Equip* (slot 0xd = EquipOcarina). Vtable
    # main+0x4b49698; +0x18 points back at Link's actor (other actors have one too).
    VT_EQUIP_OFF, EQUIP_OWNER = 0x4B49698, 0x18

    def equip_component(self):
        """Guest address of Link's equipment component, or None (scans memory, ~1 s)."""
        import numpy as np
        link = self._link_obj()
        if not link:
            return None
        c = getattr(self, "_equip_comp", None)
        if c and self.gq(c) == self.M + self.VT_EQUIP_OFF and self.gq(c + self.EQUIP_OWNER) == link:
            return c
        vt = self.M + self.VT_EQUIP_OFF
        for ga, data in self._guest_chunks(0x2000000000, 0x2400000000, writable=True):
            arr = np.frombuffer(data, dtype="<u8", count=len(data) // 8)
            for i in np.nonzero(arr == vt)[0]:
                c = ga + int(i) * 8
                if self.gq(c + self.EQUIP_OWNER) == link:
                    self._equip_comp = c
                    return c
        return None

    # The object holding Link's equipment component at +0x470 (vtable main+0x4bdfe68). The game's
    # remove-item code calls main+0xf201b0(it, 0) to re-apply all of Link's gear from PlayerStatus,
    # which also takes off gear whose slot is now empty.
    VT_EQUIP_OWNER_OFF, EQUIP_AT = 0x4BDFE68, 0x470

    def equip_owner(self):
        """Guest address of the object that holds Link's equipment component, or None."""
        import numpy as np
        comp = self.equip_component()
        if not comp:
            return None
        o = getattr(self, "_equip_owner", None)
        if o and self.gq(o) == self.M + self.VT_EQUIP_OWNER_OFF and self.gq(o + self.EQUIP_AT) == comp:
            return o
        for ga, data in self._guest_chunks(0x2000000000, 0x2400000000, writable=True):
            arr = np.frombuffer(data, dtype="<u8", count=len(data) // 8)
            for i in np.nonzero(arr == comp)[0]:
                o = ga + int(i) * 8 - self.EQUIP_AT
                if self.gq(o) == self.M + self.VT_EQUIP_OWNER_OFF:
                    self._equip_owner = o
                    return o
        return None

    def _update_health(self):
        o = self._link_obj()
        self.link_obj = o
        camp = self.gq(self.M + CAMERA_PTR_OFF)
        self.camera = self.gq(camp + 0x68) if camp else None
        o2 = self.gq((self.gq(o + 8) or 0) + 0x6A0) if o else None
        p, m = (self.gq(o2 + 0x1C10), self.gq(o2 + 0x1C18)) if o2 else (None, None)
        ok = lambda x: x and self.gq(x) == self.M + VT_PARAM_OFF  # noqa: E731
        self.hp = p + 8 if ok(p) else None
        self.hp_max = m + 8 if ok(m) else None

    def _int_valid(self):
        return self.int_base is not None and self.gq(self.int_base) == self.M + VT_INTFIELD_OFF

    def _str_valid(self):
        return self.str_base is not None and bool(self.get_str("map"))

    def _guest_chunks(self, lo, hi, size=16 << 20, writable=False):
        """Yield (guest_address, bytes) over committed, readable memory in [lo, hi)."""
        k, m, a = guestmem.k, guestmem.MBI(), self.gb + lo
        ok = WRITABLE if writable else READABLE
        while a < self.gb + hi and k.VirtualQueryEx(self.p.handle, a, ctypes.byref(m), ctypes.sizeof(m)):
            end = min(m.BaseAddress + m.RegionSize, self.gb + hi)
            if m.State == MEM_COMMIT and m.Protect in ok:
                for pos in range(a, end, size):
                    try:
                        yield pos - self.gb, self.p.read(pos, min(size, end - pos))
                    except OSError:
                        pass
            a = end

    def _find_gamedata(self):
        vt = struct.pack("<Q", self.M + VT_INTFIELD_OFF)
        anchor_hash = struct.pack("<I", INT_HASH["rupees"])
        int_found = str_found = False
        for g, data in self._guest_chunks(*GD_RANGE):
            if not str_found:
                i = data.find(STR_ANCHOR)
                while i != -1 and not str_found:
                    if i % 8 == 0:
                        base = g + i + STR_FIRST_VALUE - STR_IDX["map"] * STR_STRIDE
                        name = self._read_cstr(base + STR_IDX["map"] * STR_STRIDE)
                        if re.fullmatch(r"\w{4,}", name):
                            self.str_base, str_found = base, True
                    i = data.find(STR_ANCHOR, i + 1)
            if not int_found:
                i = data.find(anchor_hash)
                while i != -1 and not int_found:
                    idx = struct.unpack_from("<I", data, i + 4)[0] if i + 8 <= len(data) else 1 << 31
                    if i % 8 == 0 and idx < 0x8000:
                        # Objects start right after the hash table: first vtable after the entry.
                        j = data.find(vt, i)
                        if j != -1 and (j - i) < 0x20000:
                            base = g + j
                            if self.gq(base + idx * INT_STRIDE) == self.M + VT_INTFIELD_OFF:
                                table = data[max(0, i - 0x20000):j]
                                pairs = {struct.unpack_from("<I", table, o)[0]: struct.unpack_from("<I", table, o + 4)[0]
                                         for o in range(len(table) % 8, len(table) - 7, 8)}
                                self.int_by_hash = {h: i for h, i in pairs.items() if i < 0x800}
                                self.int_idx = {n: pairs[h] for n, h in INT_HASH.items() if h in pairs}
                                self.int_base, int_found = base, True
                    i = data.find(anchor_hash, i + 1)
            if int_found and str_found:
                break

    def _refresh(self):
        # After a game restart the old addresses can still pass the checks for a
        # while (stale memory), so if the save data stays missing, start over.
        stuck = self._missing_since and time.monotonic() - self._missing_since > 3
        if stuck or not self._valid_base(self.gb, self.M):
            self.state = "searching"
            self.gb, self.M = self._find_base()
            self.int_base = self.str_base = self.pos = None
            self._missing_since = None
            if not self.gb:
                self.state = "game not running"
                return
        self._update_health()
        if not self._int_valid() or not self._str_valid():
            self.state = "finding save data"
            self._find_gamedata()
        if self._int_valid() and self._str_valid():
            self.state, self._missing_since = "ok", None
        else:
            self.state = "save data not found (title screen or loading?)"
            self._missing_since = self._missing_since or time.monotonic()

    def _loop(self):
        last = None
        while not self._stop:
            try:
                self._refresh()
            except Exception as e:  # keep looking; the game may be mid-load
                self.state = f"error: {e!r}"
            if self.state != last:
                self.log(f"game: {self.state}" + (f" (guestbase {self.gb:#x}, main {self.M:#x})"
                                                 if self.state == "ok" else ""))
                last = self.state
            time.sleep(0.5)

    # ------------------------------------------------------------ public API
    @property
    def ready(self):
        return self.state == "ok"

    def get_int(self, name):
        if self.int_base is None or name not in self.int_idx:
            return None
        return self.read_i32(self.int_base + self.int_idx[name] * INT_STRIDE + INT_VALUE)

    def set_int(self, name, value):
        a = self.int_base + self.int_idx[name] * INT_STRIDE + INT_VALUE
        self.write_i32(a, value)
        self.write_i32(a + 4, value)  # second copy; the HUD follows both

    # ------------------------------------------------------------ other field types
    # GameData's type list holds (type vtable, count, objects, hash table) per field
    # type. Every type keeps the value at +0x1C with a copy at +0x20; only the object
    # size differs. Enum values are MurmurHash3 of the choice name.
    FIELD_TYPES = {"float": (0x4C1F3C0, 0x28, "<f"), "enum": (0x4C1F500, 0x38, "<I")}

    def _typed(self, kind):
        """(objects, hash -> index) for a field type, found once per save load."""
        cache = getattr(self, "_typed_cache", {})
        self._typed_cache = cache
        c = cache.get(kind)
        if c and self.gq(c[0]) == c[2]:
            return c
        import numpy as np
        vt_off, _, _ = self.FIELD_TYPES[kind]
        holder = None
        for ga, data in self._guest_chunks(*GD_RANGE):
            hits = np.nonzero(np.frombuffer(data, dtype="<u8", count=len(data) // 8) == self.int_base)[0]
            if len(hits):
                holder = ga + int(hits[0]) * 8
                break
        if holder is None:
            return None
        b = self.p.read(self.gb + holder - 0x400, 0x800)
        for o in range(0, len(b) - 32, 8):
            vt, count, objs, tbl = struct.unpack_from("<4Q", b, o)
            if vt == self.M + vt_off and 0 < count < 0x10000 and tbl < objs:
                t = self.p.read(self.gb + tbl, objs - tbl)
                idx = {}
                for i in range(0, len(t) - 7, 8):
                    h, ix = struct.unpack_from("<II", t, i)
                    if h and ix < count:
                        idx.setdefault(h, ix)
                cache[kind] = c = (objs, idx, self.gq(objs))
                return c
        return None

    def _typed_addr(self, kind, h):
        c = self.int_base is not None and self._typed(kind)
        if not c or h not in c[1]:
            return None
        return c[0] + c[1][h] * self.FIELD_TYPES[kind][1] + 0x1C

    def get_typed(self, kind, h):
        a = self._typed_addr(kind, h)
        if a is None:
            return None
        return struct.unpack(self.FIELD_TYPES[kind][2], self.p.read(self.gb + a, 4))[0]

    def set_typed(self, kind, h, value):
        a = self._typed_addr(kind, h)
        if a is None:
            raise KeyError(f"{kind} field {h:#x} not found")
        v = struct.pack(self.FIELD_TYPES[kind][2], value)
        self.p.write(self.gb + a, v + v)

    # ------------------------------------------------------------ magic
    def _magic_ok(self):
        a = getattr(self, "magic_obj", None)
        try:
            return a is not None and self.p.read(self.gb + a + MAGIC_KEYS, 8) == struct.pack(
                "<II", MAGIC_HASH, MAGIC_CAP_HASH)
        except OSError:
            return False

    def _find_magic(self):
        if self._magic_ok():
            return True
        now = time.monotonic()
        if now - getattr(self, "_magic_tried", 0) < MAGIC_RETRY:
            return False
        self._magic_tried, self.magic_obj = now, None
        pat = struct.pack("<II", MAGIC_HASH, MAGIC_CAP_HASH)
        for g, data in self._guest_chunks(*MAGIC_RANGE, writable=True):
            i = data.find(pat)
            while i != -1:
                if i % 4 == 0 and i >= MAGIC_KEYS:
                    self.magic_obj = g + i - MAGIC_KEYS
                    self.log(f"game: magic meter at {self.magic_obj:#x}")
                    return True
                i = data.find(pat, i + 1)
        return False

    def set_magic(self, value):
        """Set current magic for real (meter object + save field). Needs Magic capacity > 0 to show."""
        if self._find_magic():
            self.write_i32(self.magic_obj + MAGIC_VALUE, value)
        self.set_field(MAGIC_HASH, value)

    # Bool fields: objects 0x18 apart, value byte at +0x16 (copy at +0x17). Found
    # through GameData's type list, which also points at the Int objects.
    BOOL_STRIDE, BOOL_VALUE = 0x18, 0x16

    def _find_bools(self):
        import numpy as np
        target, holder = self.int_base, None
        for ga, data in self._guest_chunks(*GD_RANGE):
            arr = np.frombuffer(data, dtype="<u8", count=len(data) // 8)
            hits = np.nonzero(arr == target)[0]
            if len(hits):
                holder = ga + int(hits[0]) * 8
                break
        if holder is None:
            return False
        # The type list holds (vtable, count, objects, hash table) per type; the
        # Bool entry is the one whose vtable is main+VT_BOOLFIELD_OFF.
        b = self.p.read(self.gb + holder - 0x400, 0x800)
        for o in range(0, len(b) - 32, 8):
            vt, count, objs, tbl = struct.unpack_from("<4Q", b, o)
            if vt == self.M + VT_BOOLTYPE_OFF and 0 < count < 0x10000 and tbl < objs:
                t = self.p.read(self.gb + tbl, objs - tbl)
                self.bool_by_hash = {}
                for i in range(0, len(t) - 7, 8):
                    h, ix = struct.unpack_from("<II", t, i)
                    if h and ix < count:
                        self.bool_by_hash.setdefault(h, ix)
                self.bool_base = objs
                return True
        return False

    def get_bool(self, h):
        if not self._bools_ready() or h not in self.bool_by_hash:
            return None
        a = self.bool_base + self.bool_by_hash[h] * self.BOOL_STRIDE + self.BOOL_VALUE
        return self.p.read(self.gb + a, 1) != b"\0"

    def set_bool(self, h, value):
        if not self._bools_ready():
            raise RuntimeError("bool fields not found")
        a = self.bool_base + self.bool_by_hash[h] * self.BOOL_STRIDE + self.BOOL_VALUE
        self.p.write(self.gb + a, b"\1\1" if value else b"\0\0")

    def _bools_ready(self):
        if getattr(self, "bool_base", None) is None or self.gq(self.bool_base) != self.M + VT_BOOLFIELD_OFF:
            self.bool_base = None
            if self.int_base is not None:
                self._find_bools()
        return getattr(self, "bool_base", None) is not None

    def get_field(self, h):
        """An Int save field by MurmurHash3 of its name (None if unknown)."""
        i = getattr(self, "int_by_hash", {}).get(h)
        if self.int_base is None or i is None:
            return None
        return self.read_i32(self.int_base + i * INT_STRIDE + INT_VALUE)

    def set_field(self, h, value):
        i = self.int_by_hash[h]
        a = self.int_base + i * INT_STRIDE + INT_VALUE
        self.write_i32(a, value)
        self.write_i32(a + 4, value)

    def health(self):
        """(current, max) in quarter hearts, from Link's actor."""
        if not (self.hp and self.hp_max):
            return None
        return self.read_i32(self.hp), self.read_i32(self.hp_max)

    def set_health(self, v):
        if self.hp:
            self.write_i32(self.hp, v)

    def _read_cstr(self, g):
        try:
            return self.p.read(self.gb + g, 64).split(b"\0", 1)[0].decode("ascii", "replace")
        except OSError:
            return ""

    def get_str(self, name):
        return self._read_cstr(self.str_base + STR_IDX[name] * STR_STRIDE) if self.str_base else ""

    def set_str(self, name, s):
        data = s.encode("ascii")[:STR_LEN - 1]
        self.p.write(self.gb + self.str_base + STR_IDX[name] * STR_STRIDE,
                     data + b"\0" * (STR_LEN - len(data)))

    # ------------------------------------------------------------ position
    # Link's physics record: doubles (x, y, z), 0.585 above the drawn position,
    # followed by motion state that is zero while he stands still. While it is
    # zero the game ignores the position, so a move also writes WAKE after it.
    # Several look-alike records exist; the right one is whichever moves Link
    # when written, and the others are put back straight away (moving the
    # look-alikes as well once broke the camera and softlocked the game).

    def display_pos(self):
        """Link's position as drawn (floats); read-only, follows the physics record."""
        q = self.gq(self.gq(self.M + DISP_OFF)) if self.M else None
        if not q:
            return None
        try:
            return struct.unpack("<3f", self.p.read(self.gb + q + 0x29C, 12))
        except OSError:
            return None

    def _rd3(self, g):
        return struct.unpack("<3d", self.p.read(self.gb + g, 24))

    def _scan_candidates(self, lo, hi, d):
        import numpy as np
        x, y, z = d
        v, off = max([(x, 0), (y + PHYS_Y, 8), (z, 16)], key=lambda t: abs(t[0]))
        found = []
        with np.errstate(all="ignore"):
            for g, data in self._guest_chunks(lo, hi, 64 << 20, writable=True):
                arr = np.frombuffer(data, dtype="<f8", count=len(data) // 8)
                for i in np.nonzero(np.abs(arr - v) <= 3)[0]:
                    a = g + int(i) * 8 - off
                    try:
                        px, py, pz = self._rd3(a)
                    except OSError:
                        continue
                    if abs(px - x) < 3 and abs(pz - z) < 3 and abs(py - y - PHYS_Y) < 3:
                        found.append(a)
                if len(found) > 2000:
                    break
        # Keep only records that track Link closely, physics height included,
        # like oot26-tools (score over 6 samples). Records level with the drawn
        # position (camera target, render copies) must not be moved: doing so
        # broke the camera and softlocked the game.
        scores = dict.fromkeys(found, 0.0)
        for _ in range(6):
            d = self.display_pos()
            if not d:
                return []
            for a in found:
                try:
                    px, py, pz = self._rd3(a)
                    scores[a] += abs(px - d[0]) + abs(pz - d[2]) + abs(py - d[1] - PHYS_Y)
                except OSError:
                    scores[a] += 1e9
            time.sleep(0.02)
        return sorted((a for a in found if scores[a] / 6 < 0.3), key=scores.get)

    def _find_candidates(self, d):
        ranges = list(POS_RANGES)
        if self.pos:  # the record stays in the same neighbourhood between areas
            ranges.insert(0, (max(0, self.pos - POS_NEAR), self.pos + POS_NEAR))
        for lo, hi in ranges:
            t0 = time.monotonic()
            cands = self._scan_candidates(lo, hi, d)
            self.log(f"position: {len(cands)} candidates in {lo:#x}-{hi:#x} "
                     f"({time.monotonic() - t0:.2f}s)")
            if cands:
                return cands
        return []

    def move_link(self, x, y, z, settle=0.35):
        """Put Link at world (x, y, z) now, moving or standing still.

        The game must be running (not paused). Returns True once the drawn
        position has followed."""
        d0 = self.display_pos()
        if not d0:
            raise RuntimeError("Link is not loaded")
        dist = lambda a: ((a[0] - x) ** 2 + (a[1] - y) ** 2 + (a[2] - z) ** 2) ** 0.5  # noqa: E731
        if dist(d0) < 0.5:
            raise ValueError("move too small to tell which record moved Link")
        goal = dist(d0) * 0.5  # Link must have covered at least half the distance
        cands = self._find_candidates(d0)
        if self.pos in cands:  # try last time's record first
            cands.remove(self.pos)
            cands.insert(0, self.pos)
        new = struct.pack("<3d", x, y + PHYS_Y, z) + WAKE
        for a in cands:
            try:
                old = self.p.read(self.gb + a, len(new))
                self.p.write(self.gb + a, new)
            except OSError:
                continue
            t0 = time.monotonic()
            while time.monotonic() - t0 < settle:
                time.sleep(0.03)
                d = self.display_pos()
                if d and dist(d) < goal:
                    self.pos = a
                    self.log(f"position: moved Link with record {a:#x}")
                    return True
            try:  # not Link's: restore it unless the game has already rewritten it
                if self.p.read(self.gb + a, 24) == new[:24]:
                    self.p.write(self.gb + a, old)
            except OSError:
                pass
        self.log("position: no record moved Link")
        return False

    # ------------------------------------------------------------ watches

    def link_motion(self):
        """{'facing', 'move_angle', 'speed', 'y_speed', 'camera_angle'}; angles in
        degrees clockwise from +Z, None where unavailable."""
        out = dict.fromkeys(("facing", "move_angle", "speed", "y_speed", "camera_angle",
                             "camera_pitch", "camera_dist"))
        o = self.link_obj
        if o and self.gq(o) == self.M + VT_LINKOBJ_OFF:
            try:
                b = self.p.read(self.gb + o + LINK_FACING_COS, LINK_VELOCITY + 12 - LINK_FACING_COS)
                cos = struct.unpack_from("<f", b, 0)[0]
                sin = struct.unpack_from("<f", b, LINK_FACING_SIN - LINK_FACING_COS)[0]
                vx, vy, vz = struct.unpack_from("<3f", b, LINK_VELOCITY - LINK_FACING_COS)
                out["facing"] = math.degrees(math.atan2(sin, cos)) % 360
                out["speed"] = math.hypot(vx, vz)
                out["y_speed"] = vy
                if out["speed"] > 0.01:
                    out["move_angle"] = math.degrees(math.atan2(vx, vz)) % 360
            except OSError:
                pass
        if self.camera:
            try:
                px, py, pz, lx, ly, lz = struct.unpack("<6f", self.p.read(self.gb + self.camera + 0x38, 24))
                out["camera_angle"] = math.degrees(math.atan2(lx - px, lz - pz)) % 360
                out["camera_pitch"] = math.degrees(math.atan2(ly - py, math.hypot(lx - px, lz - pz)))
                d = self.display_pos()
                if d:
                    out["camera_dist"] = math.dist((px, py, pz), d)
            except OSError:
                pass
        return out

    def frame_count(self):
        """The game's own update counter (+1 per game frame)."""
        try:
            return struct.unpack("<I", self.p.read(self.gb + self.M + FRAME_COUNTER_OFF, 4))[0]
        except (OSError, TypeError):
            return None

    TYPES = {"u8": "<B", "s8": "<b", "x8": "<B", "u16": "<H", "s16": "<h", "x16": "<H",
             "u32": "<I", "s32": "<i", "x32": "<I", "f32": "<f", "f64": "<d"}

    def read_typed(self, address, type_):
        """A value at a game (guest) address, with gz's watch types (x = hex)."""
        fmt = self.TYPES[type_]
        v = struct.unpack(fmt, self.p.read(self.gb + address, struct.calcsize(fmt)))[0]
        if type_.startswith("x"):
            return f"{v:#0{struct.calcsize(fmt) * 2 + 2}x}"
        return round(v, 3) if type_.startswith("f") else v

    # ------------------------------------------------------------ levitate

    def _soft_landing(self, a, y_top):
        """Lower Link from y_top like a fall from rest, until the ground stops him.

        While levitating, the game's own falling speed (kept internally, not
        found yet) builds up to its maximum, so simply letting go dropped Link
        at full speed from the first frame. Here his height follows
        y_top - G t^2 / 2, capped at the game's top falling speed; when the
        ground pushes him above what was written, he has landed."""
        # Only ever hold Link *up* to the curve, never push him down: the game's
        # own gravity (which respects collision) does all the falling. Writing
        # heights below the floor once dropped Link straight through it.
        t0 = time.monotonic()
        hp_before = self.health() if self.protect_landing else None
        try:
            self._land(a, y_top, t0)
        finally:
            # The game bases fall damage on a falling speed kept internally (not
            # found yet; gz resets it by setting Link's velocity), so a long
            # levitate still hurts on landing. Give back any health lost on the
            # way down instead.
            if hp_before:
                time.sleep(0.6)  # damage is applied just after touching down
                hp = self.health()
                if hp and hp[0] < hp_before[0]:
                    self.set_health(hp_before[0])
                    self.log(f"levitate: landing cost {hp_before[0] - hp[0]} quarter hearts; restored")

    def _land(self, a, y_top, t0):
        held_up_by_game = 0
        while not self._lev_on and time.monotonic() - t0 < LAND_TIMEOUT:
            t = time.monotonic() - t0
            t_cap = LAND_MAX_SPEED / LAND_G  # time to reach top speed
            if t <= t_cap:
                drop = LAND_G * t * t / 2
            else:
                drop = LAND_G * t_cap * t_cap / 2 + LAND_MAX_SPEED * (t - t_cap)
            want = y_top - drop
            got = struct.unpack("<d", self.p.read(a, 8))[0]
            if got < want:      # falling faster than the curve: slow him down
                self.p.write(a, struct.pack("<d", want))
                held_up_by_game = 0
            else:               # the game has him at or above the curve (ground)
                held_up_by_game += 1
                if held_up_by_game >= 20:  # ~40 ms without needing us: landed
                    return
            time.sleep(0.002)

    def _record_ok(self, a):
        d = self.display_pos()
        try:
            x, y, z = self._rd3(a)
        except OSError:
            return False
        return bool(d) and abs(x - d[0]) < 3 and abs(z - d[2]) < 3 and abs(y - d[1] - PHYS_Y) < 3

    _lev_on = False
    _lev_gen = 0  # each press gets a new number; only the current one may switch off
    protect_landing = True  # restore health lost landing from a levitate

    def levitate_start(self, speed):
        """Start rising at `speed` units/s until levitate_stop()."""
        self._lev_speed = speed
        if not self._lev_on:
            self._lev_on = True
            self._lev_gen += 1
            threading.Thread(target=self._levitate_loop, args=(self._lev_gen,), daemon=True).start()

    def levitate_stop(self):
        self._lev_on = False

    def _levitate_loop(self, gen):
        # The game applies gravity every frame; writing the height ~1000 times a
        # second, worked out from elapsed time, means every frame sees a smoothly
        # rising value instead of a fall-then-yank (which looked juddery).
        winmm = ctypes.WinDLL("winmm")
        winmm.timeBeginPeriod(1)
        try:
            if not (self.pos and self._record_ok(self.pos)):
                d = self.display_pos()
                if not d or not self.move_link(d[0], d[1] + 1.0, d[2]):  # finds the record
                    return
            a = self.gb + self.pos + 8
            y0, t0 = self._rd3(self.pos)[1], time.monotonic()
            self.p.write(self.gb + self.pos + 24, WAKE)  # in case he's standing still
            y = y0
            while self._lev_on:
                y = y0 + self._lev_speed * (time.monotonic() - t0)
                self.p.write(a, struct.pack("<d", y))
                time.sleep(0.001)
            self._soft_landing(a, y)
        except (OSError, RuntimeError, ValueError) as e:
            self.log(f"levitate stopped: {e}")
        finally:
            if gen == self._lev_gen:  # a newer press may already be running
                self._lev_on = False
            winmm.timeEndPeriod(1)

    # ------------------------------------------------------------ warps
    # Method from oot26-tools: clear the "next map" slot, wait for the game to
    # fill it (Link entered an exit), then immediately swap in our destination.

    warp = None  # {"map", "spawn", "event", "state": armed|done|cancelled|timed out|failed}

    def arm_warp(self, map_name, spawn, event):
        """Arm a warp: the next exit Link takes leads to map_name/spawn."""
        if not self.str_base:
            raise RuntimeError("game data not found yet")
        self.cancel_warp()
        w = {"map": map_name, "spawn": spawn, "event": event, "state": "armed"}
        self.warp = w
        self.set_str("next_map", "")
        threading.Thread(target=self._warp_loop, args=(w,), daemon=True).start()

    def enter_nearest_exit(self, exits):
        """Put Link into the nearest of `exits` so an armed warp fires now.
        The game must be running. Returns True if Link was moved."""
        here = self.display_pos()
        if not (exits and here):
            return False
        x, y, z = min(exits, key=lambda e: (e[0] - here[0]) ** 2 + (e[2] - here[2]) ** 2)
        return self.move_link(x, y, z)

    def cancel_warp(self):
        w = self.warp
        if w and w["state"] == "armed":
            w["state"] = "cancelled"

    def _warp_loop(self, w):
        winmm = ctypes.WinDLL("winmm")
        winmm.timeBeginPeriod(1)  # 1 ms sleeps, so we beat the game to the slot
        try:
            deadline = time.monotonic() + WARP_TIMEOUT
            while w["state"] == "armed":
                if time.monotonic() > deadline:
                    w["state"] = "timed out"
                    return
                try:
                    if self.get_str("next_map"):
                        self.set_str("next_map", w["map"])
                        self.set_str("spawn", w["spawn"])
                        self.set_str("starter", w["spawn"])
                        self.set_str("event", w["event"])
                        w["state"] = "done"
                        return
                except OSError:
                    pass
                time.sleep(0.001)
        except Exception as e:
            w["state"] = f"failed: {e}"
        finally:
            winmm.timeEndPeriod(1)
