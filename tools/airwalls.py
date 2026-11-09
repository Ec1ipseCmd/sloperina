"""Build airwalls.json: every air wall placed in every map, with its collision shape.

    python airwalls.py            writes ../airwalls.json (needs OOTMENU_NUP and zstandard)

For each map (banc name, e.g. SacredForestMeadow_Child, the same as the "map" string in the save), every
placed actor whose name contains "AirWall": its position, rotation (radians, X Y Z) and scale from the
.bcett layout, and its shapes from the actor pack (ControllerSetParam ShapeNamePathAry, following $parent),
in the actor's own unscaled space. Box: centre (incl. OffsetTranslation) + half extents. Cylinder: two end
centres + radius. Other shapes (Polytope, PhshMesh) are counted but not stored. "mask" is the rigid body's
EnableLayerHitMask (HitOnlyPlayer, HitOnlyCharacter, ... or None = everything).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import byml  # noqa: E402
import gamefiles  # noqa: E402

OUT = os.path.join(os.path.dirname(HERE), "data", "airwalls.json")


def member(path):
    """'Work/Phive/X/Y.phive__X.gyml' -> the pack member name 'Phive/X/Y.phive__X.bgyml'."""
    p = path[5:] if path.startswith("Work/") else path
    return p[:-5] + ".bgyml" if p.endswith(".gyml") else p


def load_chain(files, name):
    """A pack BYML with its $parent chain merged (child keys win)."""
    out, seen = {}, set()
    while name and name in files and name not in seen:
        seen.add(name)
        d = byml.load(files[name])
        for k, v in d.items():
            out.setdefault(k, v)
        name = member(d["$parent"]) if "$parent" in d else None
    return out


def v3(d, default=0.0):
    return [float(d.get(k, default)) for k in ("X", "Y", "Z")] if isinstance(d, dict) else [default] * 3


_cache = {}


def actor_shapes(actor):
    """(shapes, mask, skipped) for an actor name, from its pack."""
    if actor in _cache:
        return _cache[actor]
    path = f"romfs/Pack/Actor/{actor}.pack.zs"
    result = ([], None, [])
    try:
        files = gamefiles.sarc(gamefiles.read(path))
    except KeyError:
        _cache[actor] = result
        return result
    phys = load_chain(files, f"Component/Physics/{actor}.engine__component__PhysicsParam.bgyml")
    cset_name = member(phys["ControllerSetPath"]) if "ControllerSetPath" in phys else \
        f"Phive/ControllerSetParam/{actor}.phive__ControllerSetParam.bgyml"
    cset = load_chain(files, cset_name)
    shapes, skipped = [], []
    for ref in cset.get("ShapeNamePathAry") or []:
        sp = load_chain(files, member(ref["FilePath"]))
        for b in sp.get("Box") or []:
            c, off = v3(b.get("Center")), v3(b.get("OffsetTranslation"))
            shapes.append({"type": "box", "center": [c[i] + off[i] for i in range(3)],
                           "half": v3(b.get("HalfExtents"), 1.0)})
        for c in sp.get("Cylinder") or []:
            shapes.append({"type": "cylinder", "a": v3(c.get("CenterA")), "b": v3(c.get("CenterB")),
                           "radius": float(c.get("Radius", 1.0))})
        skipped += [k for k in sp if k not in ("Box", "Cylinder", "AutoCalc", "$parent")]
    mask = None
    for ref in cset.get("RigidBodyEntityNamePathAry") or []:
        rb = load_chain(files, member(ref["FilePath"]))
        mask = rb.get("EnableLayerHitMask", mask)
    result = (shapes, mask, skipped)
    _cache[actor] = result
    return result


def main():
    n = gamefiles.nup()
    bancs = sorted(p for p in n.index if p.startswith("romfs/Banc/") and p.endswith(".bcett.byml.zs"))
    maps, skipped_total = {}, {}
    for p in bancs:
        stem = p.rsplit("/", 1)[1][:-len(".bcett.byml.zs")]
        part = stem.rsplit("_", 1)[-1]
        mapname = stem.rsplit("_", 1)[0] if part in ("Static", "Dynamic") else stem
        try:
            root = byml.load(gamefiles.read(p))
        except Exception as e:  # a file this reader can't handle: report and go on
            print(f"skip {p}: {e}")
            continue
        for a in root.get("Actors") or []:
            actor = a.get("Gyaml", "")
            if "AirWall" not in actor:
                continue
            shapes, mask, skipped = actor_shapes(actor)
            for s in skipped:
                skipped_total[s] = skipped_total.get(s, 0) + 1
            if not shapes:
                continue
            maps.setdefault(mapname, []).append({
                "actor": actor, "part": part, "mask": mask, "hash": a.get("Hash"),
                "t": [float(v) for v in a.get("Translate", [0, 0, 0])],
                "r": [float(v) for v in a.get("Rotate") or [0, 0, 0]],
                "s": [float(v) for v in a.get("Scale") or [1, 1, 1]] if isinstance(a.get("Scale"), list)
                else [float(a.get("Scale", 1.0))] * 3,
                "shapes": shapes,
            })
    with open(OUT, "w") as f:
        json.dump({"source": "game files (tools/airwalls.py)", "maps": maps}, f, separators=(",", ":"))
    total = sum(len(v) for v in maps.values())
    print(f"{total} air walls in {len(maps)} maps -> {OUT}")
    if skipped_total:
        print("shapes not stored (not box/cylinder):", skipped_total)


if __name__ == "__main__":
    main()
