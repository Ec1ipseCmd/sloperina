"""Write spawn_starters.json: for each warps.json spawn, the placed actors (other than its StartPos)
that share its name in the map's layouts.

    python tools/spawnstarters.py            (set OOTMENU_NUP; needs zstandard)

Spawns that exits lead to have the exit actor there (FieldAndDungeonEntranceEventRequestActor), which
the standard arrival event (Dm_SY_BancChangeArrive) uses as its event starter. Spawns without one
(warp-song pads, blue warps, boss rooms, restart points) are only ever entered with no starter and
their own arrival event; the native menu's warps use OcarinaWarp_Arrive for those (tools/gendata.py).
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import gamefiles  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main():
    spawns = json.loads((ROOT / "warps.json").read_text())["spawns"]
    layouts = {}
    for p in gamefiles.nup().index:
        if p.startswith("romfs/Banc/") and p.endswith(".bcett.byml.zs"):
            base = p.rsplit("/", 1)[-1][:-len(".bcett.byml.zs")]
            for part in ("_Static", "_Dynamic"):
                if base.endswith(part):
                    layouts.setdefault(base[:-len(part)], []).append(p)
    out = {}
    for m, sp in spawns.items():
        names = {}

        def walk(o):
            if isinstance(o, dict):
                if "Gyaml" in o and o.get("Gyaml") != "StartPos" and o.get("Name"):
                    names.setdefault(o["Name"], set()).add(o["Gyaml"])
                for v in o.values():
                    walk(v)
            elif isinstance(o, list):
                for v in o:
                    walk(v)

        for p in layouts.get(m, []):
            walk(byml.load(gamefiles.read(p)))
        out[m] = {pos: sorted(names.get(pos, [])) for pos, _, _ in sp}
    (ROOT / "spawn_starters.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(f"spawn_starters.json: {sum(len(v) for v in out.values())} spawns")


if __name__ == "__main__":
    main()
