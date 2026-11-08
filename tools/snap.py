"""Snapshot every live GameData field (all types) and diff two snapshots, with names where known.

    python snap.py snap <name>        save snapshots/<name>.snap.json
    python snap.py diff <old> <new>   fields whose stored bytes changed

Every field type sits in GameData's type list as (type vtable, count, objects, hash table);
objects of one type are a fixed size apart. A snapshot keeps each object's bytes after its vtable.
"""
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent))
import game  # noqa: E402
import win32  # noqa: E402
import magicwatch  # noqa: E402

SNAPS = HERE.parent / "snapshots"
NAME_FILES = [HERE.parent / "all_fields.json", HERE.parent / "rom_names.json"]


def names():
    out = {}
    for p in NAME_FILES:
        if not p.exists():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        if isinstance(d, list):  # all_fields.json (editor titles)
            for e in d:
                out.setdefault(int(e["hash"], 16), f"{e['group']}: {e['title']}")
        else:  # rom_names.json: hash -> [name, type]
            for h, (s, t) in d.items():
                out[int(h, 16)] = s
    return out


def type_list(g):
    import numpy as np
    for ga, data in g._guest_chunks(*game.GD_RANGE):
        hits = np.nonzero(np.frombuffer(data, dtype="<u8", count=len(data) // 8) == g.int_base)[0]
        if len(hits):
            holder = ga + int(hits[0]) * 8
            break
    b = g.p.read(g.gb + holder - 0x400, 0x800)
    for o in range(0, len(b) - 32, 8):
        vt, count, objs, tbl = struct.unpack_from("<4Q", b, o)
        if g.M <= vt < g.M + 0x6000000 and 0 < count < 0x10000 and 0 < objs - tbl < 0x400000:
            yield vt - g.M, count, objs, tbl


def snapshot(g):
    out = {}
    for vt, count, objs, tbl in type_list(g):
        t = g.p.read(g.gb + tbl, objs - tbl)
        idx = {}
        for i in range(0, len(t) - 7, 8):
            h, ix = struct.unpack_from("<II", t, i)
            if h and ix < count:
                idx.setdefault(ix, h)
        first = g.p.read(g.gb + objs, 0x400)
        stride = next((i for i in range(8, 0x400, 8) if first[i:i + 8] == first[:8]), None)
        if not stride:
            continue
        data = g.p.read(g.gb + objs, stride * count)
        for ix, h in idx.items():
            out["%#010x" % h] = [vt, data[ix * stride + 8:(ix + 1) * stride].hex()]
    return out


def main():
    a = sys.argv[1:]
    if a[:1] == ["snap"]:
        g = magicwatch.attach_game(lambda *_: 0, field_hash=0x76B18200)
        s = snapshot(g)
        SNAPS.mkdir(exist_ok=True)
        (SNAPS / f"{a[1]}.snap.json").write_text(json.dumps(s))
        print(f"saved {len(s)} fields as {a[1]}")
    elif a[:1] == ["diff"]:
        old, new = (json.loads((SNAPS / f"{n}.snap.json").read_text()) for n in a[1:3])
        nm = names()
        for h in sorted(set(old) & set(new), key=lambda k: nm.get(int(k, 16), "~" + k)):
            if old[h][1] != new[h][1]:
                o, n = bytes.fromhex(old[h][1]), bytes.fromhex(new[h][1])
                # show the 4-byte value slot (+0x1C from object start = +0x14 here) when present
                ov = o[0x14:0x18].hex() if len(o) >= 0x18 else o.hex()
                nv = n[0x14:0x18].hex() if len(n) >= 0x18 else n.hex()
                if len(o) >= 0x10 and o[0xE:0x10] != n[0xE:0x10] and len(o) <= 0x10:
                    ov, nv = o[0xE:0x10].hex(), n[0xE:0x10].hex()  # Bool value bytes
                print(f"{h}  {nm.get(int(h, 16), '?'):<70} {ov} -> {nv}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
