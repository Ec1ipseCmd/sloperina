"""The game's live save-data table ("GameData") in guest RAM.

progress.sav and the running game share one layout: a long run of
(hash: u32, value: u32) pairs, where hash = MurmurHash3_32(field name, seed 0).
Find the table once, and any field can be read or written by its hash.

  python gamedata.py snap <name>           save a snapshot of the table
  python gamedata.py diff <old> <new>      list fields whose value changed
  python gamedata.py get <hash>            read a field
  python gamedata.py set <hash> <value>    write a field
"""

import json
import struct
import sys
from pathlib import Path

import guestmem
import win32

# A field known to exist in every save; used to locate the table.
ANCHOR_HASH = 0xE0010A50  # wallet capacity
SNAP_DIR = Path(__file__).with_name("snapshots")


def murmur3(data, seed=0):
    if isinstance(data, str):
        data = data.encode()
    c1, c2, h, n = 0xcc9e2d51, 0x1b873593, seed, len(data)
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xffffffff  # noqa: E731
    for i in range(0, n - n % 4, 4):
        k = rotl((int.from_bytes(data[i:i + 4], "little") * c1) & 0xffffffff, 15)
        h = (rotl(h ^ ((k * c2) & 0xffffffff), 13) * 5 + 0xe6546b64) & 0xffffffff
    tail = data[n - n % 4:]
    if tail:
        k = rotl((int.from_bytes(tail, "little") * c1) & 0xffffffff, 15)
        h ^= (k * c2) & 0xffffffff
    h ^= n
    h = ((h ^ (h >> 16)) * 0x85ebca6b) & 0xffffffff
    h = ((h ^ (h >> 13)) * 0xc2b2ae35) & 0xffffffff
    return h ^ (h >> 16)


def find_anchor(ram, anchor=ANCHOR_HASH):
    """RAM offsets where the anchor hash sits at a 4-byte boundary."""
    pat, hits = struct.pack("<I", anchor), []
    for start, length in guestmem.resident_runs(ram.proc, ram.base, ram.size):
        for pos in range(start, start + length, 64 << 20):
            n = min(64 << 20, start + length - pos)
            try:
                data = ram.read(pos, n)
            except OSError:
                continue
            i = data.find(pat)
            while i != -1:
                if i % 4 == 0:
                    hits.append(pos + i)
                i = data.find(pat, i + 1)
    return hits


class GameData:
    """Window of RAM around the table, re-read on demand."""

    WINDOW = 512 << 10  # bytes either side of the anchor to treat as the table

    def __init__(self, ram, anchor_offset=None):
        self.ram = ram
        if anchor_offset is None:
            hits = find_anchor(ram)
            if len(hits) != 1:
                raise LookupError(f"expected 1 table, found {len(hits)}")
            anchor_offset = hits[0]
        self.anchor = anchor_offset
        # Pairs are 8-byte aligned relative to the anchor.
        self.start = max(0, anchor_offset - self.WINDOW)
        self.start += (anchor_offset - self.start) % 8
        self.index = {}
        self.refresh_index()

    def pairs(self):
        data = self.ram.read(self.start, 2 * self.WINDOW)
        for i in range(0, len(data) - 7, 8):
            h, v = struct.unpack_from("<II", data, i)
            yield self.start + i, h, v

    def refresh_index(self):
        self.index = {h: off for off, h, _ in self.pairs() if h}

    def get(self, h):
        return self.ram.read_int(self.index[h] + 4, 4)

    def set(self, h, value):
        self.ram.write_int(self.index[h] + 4, value & 0xffffffff, 4)


def open_table():
    pid = win32.find_pid("lichii.exe")
    if not pid:
        sys.exit("lichii.exe is not running")
    return GameData(guestmem.GuestRam(win32.Process(pid)))


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    if cmd == "diff":
        old, new = (json.loads((SNAP_DIR / f"{n}.json").read_text()) for n in args[:2])
        for h in sorted(set(old) & set(new), key=lambda k: int(k, 16)):
            if old[h] != new[h]:
                print(f"  {h}  {old[h]} -> {new[h]}")
        return
    gd = open_table()
    if cmd == "snap":
        SNAP_DIR.mkdir(exist_ok=True)
        snap = {f"{h:#010x}": v for _, h, v in gd.pairs() if h}
        (SNAP_DIR / f"{args[0]}.json").write_text(json.dumps(snap))
        print(f"table at RAM {gd.anchor:#x}; saved {len(snap)} entries as '{args[0]}'")
    elif cmd == "get":
        print(gd.get(int(args[0], 16)))
    elif cmd == "set":
        h, v = int(args[0], 16), int(args[1])
        gd.set(h, v)
        print(f"{h:#010x} = {gd.get(h)}")


if __name__ == "__main__":
    main(sys.argv[1:])
