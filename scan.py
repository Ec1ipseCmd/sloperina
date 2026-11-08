"""Value scanner for the game's RAM, like a minimal Cheat Engine.

  python scan.py new <value> [--size 1|2|4]   first scan: every place holding <value>
  python scan.py next <value>                  keep only places that now hold <value>
  python scan.py changed | unchanged           keep places that changed / stayed the same
  python scan.py show [n]                      list candidates with current values
  python scan.py write <offset> <value>        write a value (offset in hex, from 'show')

Typical use: note your rupees, `new 37`, spend or collect some, `next 42`,
repeat until a handful are left, then `write` one to confirm it's the right one.
Candidates are kept in scan_state.bin next to this file."""

import array
import json
import sys
from pathlib import Path

import guestmem
import win32

STATE = Path(__file__).with_name("scan_state.bin")
META = Path(__file__).with_name("scan_state.json")
CHUNK = 64 << 20
UNREADABLE = -(1 << 62)


def open_ram():
    pid = win32.find_pid("lichii.exe")
    if not pid:
        sys.exit("lichii.exe is not running")
    return guestmem.GuestRam(win32.Process(pid))


def save(offsets, values, size):
    with STATE.open("wb") as f:
        offsets.tofile(f)
        values.tofile(f)
    META.write_text(json.dumps({"size": size, "count": len(offsets)}))


def load():
    meta = json.loads(META.read_text())
    n = meta["count"]
    offsets, values = array.array("Q"), array.array("q")
    with STATE.open("rb") as f:
        offsets.fromfile(f, n)
        values.fromfile(f, n)
    return offsets, values, meta["size"]


def first_scan(ram, value, size):
    pattern = value.to_bytes(size, "little", signed=value < 0)
    offsets, values = array.array("Q"), array.array("q")
    for start, length in guestmem.resident_runs(ram.proc, ram.base, ram.size):
        for pos in range(start, start + length, CHUNK):
            n = min(CHUNK, start + length - pos)
            try:
                data = ram.read(pos, n)
            except OSError:
                continue
            i = data.find(pattern)
            while i != -1:
                if i % size == 0:
                    offsets.append(pos + i)
                    values.append(value)
                i = data.find(pattern, i + 1)
    return offsets, values


def current_values(ram, offsets, size):
    """Re-read every candidate, batching neighbours into one read."""
    out = array.array("q")
    i = 0
    while i < len(offsets):
        lo = offsets[i]
        j = i
        while j + 1 < len(offsets) and offsets[j + 1] + size - lo <= 1 << 20:
            j += 1
        try:
            data = ram.read(lo, offsets[j] + size - lo)
            for k in range(i, j + 1):
                o = offsets[k] - lo
                out.append(int.from_bytes(data[o:o + size], "little", signed=True))
        except OSError:
            out.extend([UNREADABLE] * (j - i + 1))  # never matches a real value
        i = j + 1
    return out


def main(argv):
    if not argv:
        sys.exit(__doc__)
    cmd, args = argv[0], argv[1:]
    ram = open_ram()

    if cmd == "new":
        size = int(args[args.index("--size") + 1]) if "--size" in args else 4
        offsets, values = first_scan(ram, int(args[0]), size)
        save(offsets, values, size)
        print(f"{len(offsets)} candidates ({size}-byte)")
        return

    offsets, values, size = load()
    if cmd in ("next", "changed", "unchanged"):
        now = current_values(ram, offsets, size)
        keep = {
            "next": lambda old, new: new == int(args[0]),
            "changed": lambda old, new: new != old,
            "unchanged": lambda old, new: new == old,
        }[cmd]
        o2, v2 = array.array("Q"), array.array("q")
        for off, old, new in zip(offsets, values, now):
            if keep(old, new):
                o2.append(off)
                v2.append(new)
        save(o2, v2, size)
        print(f"{len(o2)} candidates left (was {len(offsets)})")
        if len(o2) <= 20:
            argv = ["show"]
            offsets, values = o2, v2
        else:
            return

    if cmd == "show" or argv == ["show"]:
        limit = int(args[0]) if cmd == "show" and args else 20
        now = current_values(ram, offsets[:limit], size)
        for off, v in zip(offsets[:limit], now):
            print(f"  {off:#011x}  {v}")
        if len(offsets) > limit:
            print(f"  ... {len(offsets) - limit} more")
    elif cmd == "write":
        off, value = int(args[0], 16), int(args[1])
        ram.write_int(off, value, size, signed=value < 0)
        print(f"wrote {value} at {off:#x}")


if __name__ == "__main__":
    main(sys.argv[1:])
