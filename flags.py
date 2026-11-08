"""Search and set the game's yes/no flags (events, conversations, places, quest, dungeon) live.

Names come from all_fields.json: every Bool field the onif save editor knows (3,575), mapped
to its in-game hash by exporting the editor's demo save with known changes and diffing.

    python flags.py search gohma boss          flags whose name has all the words, with live values
    python flags.py set 0x3419ccb3 on          one flag by hash (or exact name)
    python flags.py chain "Get Stone Of Forest" on   every event in a story chain
    python flags.py backup                     copy the save folder to save_backups/<time>

Live edits change the running game only; the game keeps them once it saves.
"""
import json
import shutil
import sys
import time
from pathlib import Path

import game
import win32

HERE = Path(__file__).parent
FIELDS = json.loads((HERE / "all_fields.json").read_text(encoding="utf-8"))
DOT = " · "  # the editor's "chain · step" separator
SAVE_ROOT = Path.home() / "AppData/Roaming/eden/nand/user/save/0000000000000000"


def name(e):
    return f"{e['group']}: {e['title'].replace(DOT, ' / ')}"


def connect():
    pid = win32.find_pid("lichii.exe")
    if not pid:
        raise SystemExit("lichii.exe isn't running")
    g = game.Game(win32.Process(pid))
    for _ in range(60):
        if g.ready and g._bools_ready():
            return g
        time.sleep(0.5)
    raise SystemExit(f"save data not found ({g.state}); load a save first")


def find(key):
    key = key.strip()
    if key.lower().startswith("0x"):
        return [e for e in FIELDS if e["hash"] == key.lower()]
    return [e for e in FIELDS if e["title"] == key or name(e) == key]


def setmany(g, entries, on):
    n = 0
    for e in entries:
        h = int(e["hash"], 16)
        if h in g.bool_by_hash:
            g.set_bool(h, on)
            n += 1
        else:
            print("  not in this game version:", name(e))
    return n


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    cmd = a[0]
    if cmd == "backup":
        dst = HERE / "save_backups" / time.strftime("%Y%m%d-%H%M%S")
        src = next(p for p in SAVE_ROOT.glob("*/0400486024DEC000"))
        shutil.copytree(src, dst)
        print("saved to", dst)
        return
    g = connect()
    if cmd == "search":
        words = [w.lower() for w in a[1:]]
        hits = [e for e in FIELDS if all(w in name(e).lower() for w in words)]
        for e in hits[:200]:
            v = g.get_bool(int(e["hash"], 16))
            print(f"{e['hash']}  [{'x' if v else ' ' if v is not None else '?'}] {name(e)}")
        print(f"{len(hits)} match(es)" + (" (first 200 shown)" if len(hits) > 200 else ""))
    elif cmd == "set":
        es = find(a[1])
        if not es:
            raise SystemExit("no such flag: " + a[1])
        on = a[2].lower() in ("on", "1", "true", "yes")
        print(f"set {setmany(g, es, on)} flag(s) {'on' if on else 'off'}: {name(es[0])}")
    elif cmd == "chain":
        es = [e for e in FIELDS if e["group"] == "Played events" and e["title"].startswith(a[1] + DOT)]
        if not es:
            raise SystemExit("no events in chain: " + a[1])
        on = a[2].lower() in ("on", "1", "true", "yes")
        print(f"set {setmany(g, es, on)} event(s) {'on' if on else 'off'} in {a[1]}")
    else:
        raise SystemExit(__doc__)
    g.close()


if __name__ == "__main__":
    main()
