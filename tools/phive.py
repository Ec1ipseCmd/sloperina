"""Read Phive collision (Havok hknp) files: .bphsh shapes and .bphsc static compound bodies.

    python phive.py <romfs path | local file>        summary of a .bphsh / .bphsc (local files: already decompressed)
    python phive.py types <romfs path | local file>   dump the Havok type layouts of every tagfile in the file
    python phive.py map <Area> <Map> [out.json]       world-space triangles of every tile of a map, as JSON
                                                      e.g. map KokiriForest KokiriForest_Child
    python phive.py obj <Area> <Map> out.obj          the same, as a Wavefront .obj (for a quick look in Blender)
      options: --all        keep everything (sensors, enemy-only walls, Rough layer, unplaced bodies), tagged
               --dir <dir>  read the tiles from <dir> (decompressed, romfs names without .zs) instead of the dump

Romfs paths need OOTMENU_NUP and zstandard (see gamefiles.py); a path to a local, decompressed file doesn't.

Format (this game: Havok SDK "20250200", Phive file version 0x0202 for bphsc, 0x0100 for bphsh):
  Phive header: "Phive\\0", u16 version, BOM 0xFFFE, u8 file type (0 shape, 1 navmesh, 2 static compound,
  3 cloth), u8 N, u32 offset[N], u32 size[N] (padded to 0x10). Sections are found by content: a Havok
  tagfile ("TAG0" at +4) or a nested Phive file ("Phive" at +0).
  Havok tagfile: big-endian chunk headers (u32 size|flags<<30, 4cc). TAG0{SDKV, DATA, TYPE{TPTR, TST1, TNA1,
  FST1, TBDY, THSH, TPAD}, INDX{ITEM}}. No PTCH: every pointer/array is relative (hkRelArrayView = s32 offset
  from the field + s32 count; hkRelArray/hkRelPtr = s64 offset from the field). ITEM entries (u32 type|flags<<24,
  u32 data offset, u32 count) name the type of each object, which is how shapes are dispatched here.
  .bphsh sections: [0] tagfile (one hknpShape, usually hknpMeshShape), [1] material table (16 B each: u32 material
  (PhiveConfig MaterialCollection), u32 sub-material, u64 user shape-tag mask), [2] layer hit masks (u64, u64).
  .bphsc sections: [0] 0xCAB0-byte header (u32 counts at +0xC000; tile name at +0xCA20), [1] actors (32 B: u32,
  u32, u64 placed-actor Hash (as in the .bcett), u32 SRT hash), [2] u64 pairs, [3] u16 pairs, [4] instance info
  (20 B: u32 index, u8 isSensor, u8 layer, u8 subLayer?, u8 ?, u16 actor index, u16 instance index in its
  compound, u32 first material, u32 material count), [5] materials (as .bphsh), [6] layer hit masks; then one
  tagfile per layer holding an hknpCompoundShape (instances: quaternion, translation, scale, shape, shapeTag =
  index into [4]), then small 0x50/0x30 blocks and a nested .bphsh (the tile's merged ground mesh).
  hknpMeshShape: geometry sections; each has u16x3 vertices, decoded as
  (sectionOffset[i] + v[i]) * vertexConversionUtil.bitScale16Inv[i], and primitives of four u8 vertex indices
  (a, b, c, d): triangle (a, b, c), plus (a, c, d) when d != c (a quad).
  Convex shapes (box, cylinder, polytope) all carry an hknpConvexHull (vertices + faces); faces are fanned.
  The real surface is the hull grown by convexRadius (+0x68, usually 0.05): the hull drawn here is that much
  inside it. Capsule = 2 hull vertices + radius, sphere = 1 vertex + radius: approximated with 8-sided tubes.
Coordinates are the game's world units (metres); +Y is up.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PHIVE_TYPES = {0: "shape", 1: "navmesh", 2: "static compound", 3: "cloth"}

# PhiveConfig (romfs/Pack/Bootup.Nin_Ounce_NVN2.pack.zs : Phive/Config/PhiveConfig.byml) names, this version.
MATERIALS = [
    "Undefined", "Soil", "Grass", "Sand", "HeavySand", "Snow", "HeavySnow", "Stone", "StoneSlip", "StoneNoSlip",
    "Metal", "MetalSlip", "MetalNoSlip", "WireNet", "Wood", "Ice", "Cloth", "Glass", "Bone", "Rope", "Character",
    "Ragdoll", "Meat", "Vegetable", "Bomb", "MagicBall", "Barrier", "AirWall", "Water", "HotWater", "IceWater",
    "Lava", "Bog", "ContaminatedWater", "Gas", "InvalidateRestartPos", "HorseSpeedLimit",
    "ForbidDynamicCuttingAreaForHugeCharacter", "ForbidHorseReturnToSafePosByClimate", "WaterSlip",
    "HorseDeleteImmediatelyOnDeath", "DropItem", "Skulltula", "PoisonBog", "WoodSlip", "IceNoSlip"]
LAYERS_ENTITY = [
    "Object", "SmallObject", "GroundObject", "Player", "Character", "Water", "GroundHighRes",
    "GroundHighResSmooth", "GroundHighResRough", "GroundStaticTree", "HitOnlyGround", "CharacterSight",
    "CameraSight", "CameraBody", "NoHit", "CustomReceiver", "Fluid", "Smoke", "Gas", "LowTree", "IK",
    "CameraAlphaCtrlCheck"]
LAYERS_SENSOR = [
    "Object", "SmallObject", "Player", "Enemy", "NPC", "Animal", "TgtHitAll", "PlayerAttack", "EnemyAttack",
    "NPCAttack", "AttackAll", "ChemicalObject", "ChemicalElement", "InDoor", "StaticGeneralArea", "QueryOnly",
    "Tree", "Camera", "NoHit", "CustomReceiver", "PerceptionSensor", "PerceptionEmitter", "Fluid",
    "ForbidAutoPlacementArea", "SpecialChemical", "GeneralArea", "EnemyPerceptionSightNoThrough"]
PLAYER_LAYER_BIT = 1 << LAYERS_ENTITY.index("Player")


def name_of(table, i):
    return table[i] if 0 <= i < len(table) else str(i)


# ---------------------------------------------------------------------------------------------------- Phive


def phive_sections(data, base=0):
    """(file type, [(offset, size)]) of a Phive file starting at data[base]."""
    if data[base:base + 6] != b"Phive\0":
        raise ValueError("not a Phive file (magic %r)" % data[base:base + 6])
    ftype, n = data[base + 10], data[base + 11]
    offs = struct.unpack_from("<%dI" % n, data, base + 12)
    sizes = struct.unpack_from("<%dI" % n, data, base + 12 + 4 * n)
    return ftype, [(base + o, s) for o, s in zip(offs, sizes)]


def is_tagfile(data, off):
    return data[off + 4:off + 8] == b"TAG0"


# ---------------------------------------------------------------------------------------------------- Havok tagfile


def varint(d, p):
    b = d[p]
    if b < 0x80:
        return b, p + 1
    if b < 0xC0:
        return int.from_bytes(d[p:p + 2], "big") & 0x3FFF, p + 2
    if b < 0xE0:
        return int.from_bytes(d[p:p + 3], "big") & 0x1FFFFF, p + 3
    if b < 0xE8:
        return int.from_bytes(d[p:p + 4], "big") & 0x7FFFFFF, p + 4
    if b < 0xF0:
        return int.from_bytes(d[p:p + 5], "big") & 0x7FFFFFFFF, p + 5
    if b < 0xF8:
        return int.from_bytes(d[p:p + 8], "big") & 0x7FFFFFFFFFFFFFF, p + 8
    if b == 0xF8:
        return int.from_bytes(d[p + 1:p + 6], "big"), p + 6
    if b == 0xF9:
        return int.from_bytes(d[p + 1:p + 9], "big"), p + 9
    raise ValueError("bad varint byte %#x at %#x" % (b, p))


def _chunks(d, p, end):
    out = {}
    while p + 8 <= end:
        h = struct.unpack_from(">I", d, p)[0]
        size = h & 0x3FFFFFFF
        if size < 8:
            break
        tag = d[p + 4:p + 8].decode("latin1")
        out[tag] = _chunks(d, p + 8, p + size) if h >> 30 == 0 else (p + 8, p + size)
        p += size
    return out


class Tagfile:
    """A Havok tagfile at data[off]: type names, the ITEM list, and object lookup by data offset."""

    def __init__(self, data, off):
        self.d = data
        size = struct.unpack_from(">I", data, off)[0] & 0x3FFFFFFF
        self.chunks = _chunks(data, off, off + size)["TAG0"]
        self.sdk = data[slice(*self.chunks["SDKV"])].decode("latin1")
        self.base = self.chunks["DATA"][0]
        t = self.chunks["TYPE"]
        self.tstr = [x.decode("latin1") for x in data[slice(*t["TST1"])].split(b"\0")]
        self.fstr = [x.decode("latin1") for x in data[slice(*t["FST1"])].split(b"\0")]
        p, _ = t["TNA1"]
        n, p = varint(data, p)
        self.types = [None]
        for _ in range(1, n):
            ni, p = varint(data, p)
            tc, p = varint(data, p)
            tpl = []
            for _ in range(tc):
                a, p = varint(data, p)
                b, p = varint(data, p)
                tpl.append((self.tstr[a], b))
            self.types.append({"name": self.tstr[ni], "tpl": tpl})
        p, e = self.chunks["INDX"]["ITEM"]
        self.items = []
        while p < e:
            fl, o, c = struct.unpack_from("<III", data, p)
            p += 12
            self.items.append((fl & 0xFFFFFF, fl >> 24, self.base + o, c))
        # objects (ITEM flag 0x10) by absolute offset -> type name
        self.objects = {o: self.tname(t) for t, f, o, c in self.items if f & 0x10}

    def tname(self, i):
        if not i:
            return "-"
        if i >= len(self.types):
            return "?%d" % i
        T = self.types[i]
        s = T["name"]
        if T["tpl"]:
            s += "<" + ",".join(self.tname(v) if k.startswith("t") else str(v) for k, v in T["tpl"]) + ">"
        return s

    def root(self):
        """(offset, type name) of the first object (the top-level shape)."""
        t, f, o, c = self.items[1]
        return o, self.tname(t)

    def type_bodies(self):
        """Parse TBDY (layouts). Only for the 'types' dump; best effort (this version's attribute flag is unknown)."""
        d, t = self.d, self.chunks["TYPE"]
        p, e = t["TBDY"]
        while p < e:
            ti, p = varint(d, p)
            if ti == 0:
                continue
            T = self.types[ti]
            T["parent"], p = varint(d, p)
            fl, p = varint(d, p)
            T["flags"] = fl
            if fl & 1:
                T["format"], p = varint(d, p)
            if fl & 2:
                T["subtype"], p = varint(d, p)
            if fl & 4:
                T["version"], p = varint(d, p)
            if fl & 8:
                T["size"], p = varint(d, p)
                T["align"], p = varint(d, p)
            if fl & 0x10:
                T["tflags"], p = varint(d, p)
            if fl & 0x20:
                m, p = varint(d, p)
                mem = []
                for _ in range(m):
                    a, p = varint(d, p)
                    f, p = varint(d, p)
                    if f & 0x80:  # seen on hknpShapeMassProperties.compressedMassProperties: one more varint
                        _, p = varint(d, p)
                    o, p = varint(d, p)
                    ty, p = varint(d, p)
                    mem.append((self.fstr[a] if a < len(self.fstr) else "?%d" % a, f, o, ty))
                T["members"] = mem
            if fl & 0x40:
                m, p = varint(d, p)
                T["interfaces"] = []
                for _ in range(m):
                    a, p = varint(d, p)
                    b, p = varint(d, p)
                    T["interfaces"].append((a, b))
            if fl & 0x80:
                raise ValueError("type %d: attribute flag not understood; stopped" % ti)


def rel_view(d, p):
    """hkRelArrayView<T,int> at p -> (absolute offset, count)."""
    o, n = struct.unpack_from("<ii", d, p)
    return p + o, n


def rel_array(d, p):
    """hkRelArray<T> at p -> (absolute offset, count)."""
    o, n = struct.unpack_from("<qi", d, p)
    return p + o, n


def rel_ptr(d, p):
    o = struct.unpack_from("<q", d, p)[0]
    return p + o if o else None


# ---------------------------------------------------------------------------------------------------- shapes
# A decoded shape is a list of (v0, v1, v2, tag): local-space triangles; tag is the mesh's per-primitive
# shape tag (0 when the shape has none).


def mesh_shape(d, o):
    """hknpMeshShape at o -> triangles."""
    inv = struct.unpack_from("<3f", d, o + 0x50)  # vertexConversionUtil.bitScale16Inv
    tt, ntt = rel_view(d, o + 0x60)
    tagtable = [struct.unpack_from("<IH", d, tt + 8 * i) for i in range(ntt)]
    gs, ng = rel_view(d, o + 0x88)
    tris = []
    for si in range(ng):
        g = gs + 0x40 * si
        prim, nprim = rel_view(d, g + 0x08)
        vb, nvb = rel_view(d, g + 0x10)
        so = struct.unpack_from("<3i", d, g + 0x20)
        if so == (0x7FFFFFFF,) * 3:  # section stored unquantised: hkFloat3 vertices
            nv = nvb // 12
            raw = struct.unpack_from("<%df" % (3 * nv), d, vb)
            verts = [(raw[3 * j], raw[3 * j + 1], raw[3 * j + 2]) for j in range(nv)]
        else:
            nv = nvb // 6
            raw = struct.unpack_from("<%dH" % (3 * nv), d, vb)
            verts = [((so[0] + raw[3 * j]) * inv[0], (so[1] + raw[3 * j + 1]) * inv[1],
                      (so[2] + raw[3 * j + 2]) * inv[2]) for j in range(nv)]
        for pi in range(nprim):
            a, b, c, e = d[prim + 4 * pi:prim + 4 * pi + 4]
            key = (si << 9) | (pi << 1)
            tag = _tag_for_key(tagtable, key)
            tris.append((verts[a], verts[b], verts[c], tag))
            if e != c:
                tris.append((verts[a], verts[c], verts[e], tag))
    return tris


def _tag_for_key(table, key):
    """Shape tag for a primitive key: the table holds (first key, tag) runs, ended by tag 0xFFFF."""
    tag = 0
    for k, t in table:
        if k > key:
            break
        tag = t
    return 0 if tag == 0xFFFF else tag


def convex_shape(d, o):
    """hknpConvexShape (and subclasses: box, cylinder, ...) at o -> triangles of its hull."""
    vs, nv = rel_view(d, o + 0x38)
    fs, nf = rel_view(d, o + 0x48)
    ix, ni = rel_view(d, o + 0x50)
    verts = [struct.unpack_from("<3f", d, vs + 12 * i) for i in range(nv)]
    if nf == 0 and nv == 2:  # capsule: a segment + convexRadius
        return capsule_tris(verts[0], verts[1], struct.unpack_from("<f", d, o + 0x68)[0])
    if nf == 0 and nv == 1:  # sphere: a point + convexRadius; drawn as a capsule along Y
        r = struct.unpack_from("<f", d, o + 0x68)[0]
        c = verts[0]
        return capsule_tris((c[0], c[1] - 1e-3, c[2]), (c[0], c[1] + 1e-3, c[2]), r)
    idx = d[ix:ix + ni]
    tris = []
    for f in range(nf):
        first, n = struct.unpack_from("<HB", d, fs + 4 * f)
        ring = [verts[idx[first + k]] for k in range(n)]
        for k in range(1, n - 1):
            tris.append((ring[0], ring[k], ring[k + 1], 0))
    return tris


def capsule_tris(a, b, r, n=8):
    """A capsule as an n-sided tube from a to b with pointed (cone) caps reaching r past each end."""
    ax = [b[i] - a[i] for i in range(3)]
    ln = math.sqrt(sum(x * x for x in ax)) or 1.0
    ax = [x / ln for x in ax]
    ref = (1.0, 0.0, 0.0) if abs(ax[0]) < 0.9 else (0.0, 1.0, 0.0)
    u = [ax[1] * ref[2] - ax[2] * ref[1], ax[2] * ref[0] - ax[0] * ref[2], ax[0] * ref[1] - ax[1] * ref[0]]
    lu = math.sqrt(sum(x * x for x in u))
    u = [x / lu for x in u]
    w = [ax[1] * u[2] - ax[2] * u[1], ax[2] * u[0] - ax[0] * u[2], ax[0] * u[1] - ax[1] * u[0]]
    ring = lambda c, k: tuple(c[i] + r * (math.cos(2 * math.pi * k / n) * u[i] + math.sin(2 * math.pi * k / n) * w[i])
                              for i in range(3))  # noqa: E731
    tip_a = tuple(a[i] - r * ax[i] for i in range(3))
    tip_b = tuple(b[i] + r * ax[i] for i in range(3))
    tris = []
    for k in range(n):
        a0, a1, b0, b1 = ring(a, k), ring(a, k + 1), ring(b, k), ring(b, k + 1)
        tris += [(a0, b0, b1, 0), (a0, b1, a1, 0), (tip_a, a0, a1, 0), (tip_b, b1, b0, 0)]
    return tris


def quat_rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # t = 2 * cross(q.xyz, v); v' = v + w*t + cross(q.xyz, t)
    tx, ty, tz = 2 * (y * vz - z * vy), 2 * (z * vx - x * vz), 2 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx))


def compound_instances(d, o):
    """hknpCompoundShape at o -> [dict(q, t, s, shape offset, shapeTag, leaf)] (free slots skipped)."""
    es, n = rel_array(d, o + 0x38)
    out = []
    for i in range(n):
        p = es + 64 * i
        q = struct.unpack_from("<4f", d, p)
        t = struct.unpack_from("<3f", d, p + 0x10)
        leaf = struct.unpack_from("<I", d, p + 0x1C)[0]
        s = struct.unpack_from("<3f", d, p + 0x20)
        tag = struct.unpack_from("<H", d, p + 0x2C)[0]
        shape = rel_ptr(d, p + 0x30)
        if shape is None:
            continue
        out.append({"index": i, "q": q, "t": t, "s": s, "shape": shape, "shapeTag": tag, "leaf": leaf})
    return out


class ShapeDecoder:
    """Decodes shapes inside one tagfile, caching the local triangles of shared shapes."""

    def __init__(self, tf):
        self.tf = tf
        self.cache = {}
        self.unknown = {}

    def local(self, o):
        if o in self.cache:
            return self.cache[o]
        kind = self.tf.objects.get(o, "?")
        d = self.tf.d
        if kind == "hknpMeshShape":
            tris = mesh_shape(d, o)
        elif kind in ("hknpConvexShape", "hknpBoxShape", "hknpCylinderShape", "hknpConvexPolytopeShape",
                      "hknpCapsuleShape", "hknpSphereShape"):
            tris = convex_shape(d, o)
        elif kind == "hknpCompoundShape":
            tris = []
            for inst in compound_instances(d, o):
                tris += transform(self.local(inst["shape"]), inst)
        else:
            self.unknown[kind] = self.unknown.get(kind, 0) + 1
            tris = []
        self.cache[o] = tris
        return tris


def transform(tris, inst):
    q, t, s = inst["q"], inst["t"], inst["s"]

    def f(v):
        r = quat_rotate(q, (v[0] * s[0], v[1] * s[1], v[2] * s[2]))
        return (r[0] + t[0], r[1] + t[1], r[2] + t[2])
    cache = {}
    out = []
    for a, b, c, tag in tris:
        out.append((cache.get(a) or cache.setdefault(a, f(a)), cache.get(b) or cache.setdefault(b, f(b)),
                    cache.get(c) or cache.setdefault(c, f(c)), tag))
    return out


# ---------------------------------------------------------------------------------------------------- files


def decode_bphsh(data, base=0):
    """A .bphsh -> dict(kind, tris, materials, layermasks, sdk)."""
    ftype, secs = phive_sections(data, base)
    tf_off = next(o for o, s in secs if s and is_tagfile(data, o))
    tf = Tagfile(data, tf_off)
    root, kind = tf.root()
    dec = ShapeDecoder(tf)
    tris = dec.local(root)
    mats, masks = [], []
    rest = [(o, s) for o, s in secs if s and o != tf_off]
    if len(rest) >= 1:
        o, s = rest[0]
        mats = [struct.unpack_from("<IIQ", data, o + 16 * i) for i in range(s // 16)]
    if len(rest) >= 2:
        o, s = rest[1]
        masks = [struct.unpack_from("<QQ", data, o + 16 * i) for i in range(s // 16)]
    return {"kind": kind, "tris": tris, "materials": mats, "layermasks": masks, "sdk": tf.sdk,
            "unknown": dec.unknown}


def decode_bphsc(data):
    """A .bphsc -> dict(name, counts, actors, info, materials, layermasks, layers=[...], ground=bphsh dict|None).

    Each entry of layers: dict(section, kind, instances=[instance dicts + 'info', 'tris' (world space)]).
    """
    ftype, secs = phive_sections(data)
    if ftype != 2:
        raise ValueError("not a static compound (type %d)" % ftype)
    h, hs = secs[0]
    name = data[h + 0xCA20:data.index(b"\0", h + 0xCA20)].decode("latin1")
    counts = struct.unpack_from("<8I", data, h + 0xC000)
    n_act, n_info, n_mat = counts[4], counts[5], counts[6]

    def table(i, fmt, n):
        o, s = secs[i]
        size = struct.calcsize(fmt)
        return [struct.unpack_from(fmt, data, o + size * k) for k in range(n)] if s else []
    actors = table(1, "<IIQI", n_act)
    info = table(4, "<I4BHHII", n_info)
    mats = table(5, "<IIQ", n_mat)
    masks = table(6, "<QQ", n_mat)
    out = {"name": name, "counts": counts, "actors": actors, "info": info, "materials": mats,
           "layermasks": masks, "layers": [], "ground": None, "unknown": {}, "other": []}
    for si, (o, s) in enumerate(secs):
        if not s or si <= 6:
            continue
        if is_tagfile(data, o):
            tf = Tagfile(data, o)
            root, kind = tf.root()
            dec = ShapeDecoder(tf)
            insts = compound_instances(data, root) if kind == "hknpCompoundShape" else []
            for inst in insts:
                inst["kind"] = tf.objects.get(inst["shape"], "?")
                inst["info"] = info[inst["shapeTag"]] if inst["shapeTag"] < len(info) else None
                inst["tris"] = transform(dec.local(inst["shape"]), inst)
            for k, v in dec.unknown.items():
                out["unknown"][k] = out["unknown"].get(k, 0) + v
            first = insts[0]["info"] if insts and insts[0]["info"] else None
            key = (first[1], first[2]) if first else None  # (isSensor, layer)
            # A second entity compound on a layer already seen holds bodies in local space (e.g. 1 instance at
            # the origin in InsideTheDekuTree): placed at run time by their actor, not in world space here.
            extra = bool(key and not key[0] and any(L["key"] == key for L in out["layers"]))
            out["layers"].append({"section": si, "kind": kind, "instances": insts, "key": key, "extra": extra})
        elif data[o:o + 6] == b"Phive\0":
            out["ground"] = decode_bphsh(data, o)
            out["ground"]["section"] = si
        elif s == 0x30:
            # placement of the nested ground mesh: scale xyz, pad, then (from +0x18) translation x y z, u32 6
            f = struct.unpack_from("<12f", data, o)
            out["groundXform"] = {"s": f[0:3], "t": f[6:9], "q": (0.0, 0.0, 0.0, 1.0)}
        else:
            # Not decoded. Section 7 (0x50, zero so far) and a block after the tagfiles that looks like water
            # volumes: records starting (0, 0, 1.0?, scale xyz, rotation xyz (radians), translation xyz,
            # u32 material 28 = Water, ...) but the record size varies (0x48/0x50) - not worked out.
            out["other"].append((si, data[o:o + s]))
    if out["ground"] and "groundXform" in out:
        out["ground"]["tris"] = transform(out["ground"]["tris"], out["groundXform"])
    return out


def instance_props(sc, inst):
    """(layer name, is sensor, [material names], hits player) for a static compound instance."""
    inf = inst.get("info")
    if not inf:
        return "?", False, [], True
    _, sensor, layer, _, _, _, _, m0, mn = inf
    lname = name_of(LAYERS_SENSOR if sensor else LAYERS_ENTITY, layer)
    mats = [name_of(MATERIALS, sc["materials"][m][0]) for m in range(m0, m0 + mn) if m < len(sc["materials"])]
    hits = any(sc["layermasks"][m][0] & PLAYER_LAYER_BIT for m in range(m0, m0 + mn) if m < len(sc["layermasks"]))
    return lname, bool(sensor), mats, hits and not sensor


def bbox(tris):
    if not tris:
        return None
    lo = [min(min(t[k][i] for k in range(3)) for t in tris) for i in range(3)]
    hi = [max(max(t[k][i] for k in range(3)) for t in tris) for i in range(3)]
    return lo, hi


def classify(tri):
    """'floor' / 'wall' / 'ceiling' by the face normal's Y (threshold: 0.5, about 60 degrees from vertical)."""
    a, b, c, _ = tri
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = math.sqrt(nx * nx + ny * ny + nz * nz)
    if ln == 0:
        return "degenerate"
    ny /= ln
    return "floor" if ny > 0.5 else "ceiling" if ny < -0.5 else "wall"


# ---------------------------------------------------------------------------------------------------- CLI


def load(path):
    if os.path.exists(path):
        return open(path, "rb").read()
    import gamefiles
    return gamefiles.read(path)


def fmt_box(b):
    if not b:
        return "(empty)"
    lo, hi = b
    return "x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f" % (lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])


def summary(path):
    data = load(path)
    ftype, secs = phive_sections(data)
    print("%s: Phive %s, version %04x, %d bytes, sections %s" % (
        path, PHIVE_TYPES.get(ftype, ftype), struct.unpack_from(">H", data, 6)[0], len(data),
        [(i, hex(o), hex(s)) for i, (o, s) in enumerate(secs) if s]))
    if ftype == 0:
        sh = decode_bphsh(data)
        print("  shape %s (Havok %s): %d triangles, %s" % (sh["kind"], sh["sdk"], len(sh["tris"]), fmt_box(bbox(sh["tris"]))))
        print("  materials:", [(name_of(MATERIALS, m), sub, "%#x" % tags) for m, sub, tags in sh["materials"]])
        print("  layer masks:", ["%#x/%#x" % m for m in sh["layermasks"]])
        tags = {}
        for t in sh["tris"]:
            tags[t[3]] = tags.get(t[3], 0) + 1
        print("  triangles per shape tag:", tags)
        if sh["unknown"]:
            print("  NOT decoded:", sh["unknown"])
    elif ftype == 2:
        sc = decode_bphsc(data)
        print("  tile %s: counts %s; %d actors, %d instance infos, %d materials" % (
            sc["name"], sc["counts"], len(sc["actors"]), len(sc["info"]), len(sc["materials"])))
        total = 0
        for L in sc["layers"]:
            tris = [t for i in L["instances"] for t in i["tris"]]
            total += len(tris)
            kinds, lay = {}, {}
            for i in L["instances"]:
                kinds[i["kind"]] = kinds.get(i["kind"], 0) + 1
                ln, sensor, mats, hits = instance_props(sc, i)
                key = ("sensor:" if sensor else "") + ln
                lay[key] = lay.get(key, 0) + 1
            print("  section %d %s%s: %d instances %s, layers %s, %d triangles, %s" % (
                L["section"], L["kind"], " (EXTRA: local space, not placed)" if L["extra"] else "",
                len(L["instances"]), kinds, lay, len(tris), fmt_box(bbox(tris))))
        g = sc["ground"]
        if g:
            total += len(g["tris"])
            print("  section %d nested shape %s: %d triangles, %s, materials %s" % (
                g["section"], g["kind"], len(g["tris"]), fmt_box(bbox(g["tris"])),
                [name_of(MATERIALS, m[0]) for m in g["materials"]]))
        if "groundXform" in sc:
            print("  ground mesh placed at", sc["groundXform"]["t"], "scale", sc["groundXform"]["s"])
        for si, raw in sc["other"]:
            print("  section %d: %d bytes not decoded: %s" % (si, len(raw), raw[:0x30].hex()))
        if sc["unknown"]:
            print("  NOT decoded:", sc["unknown"])
        print("  total triangles: %d" % total)
    else:
        print("  (no decoder for this Phive type)")


def dump_types(path):
    data = load(path)
    _, secs = phive_sections(data)
    for si, (o, s) in enumerate(secs):
        if not s:
            continue
        if data[o:o + 6] == b"Phive\0":
            _, sub = phive_sections(data, o)
            todo = [x for x, y in sub if y and is_tagfile(data, x)]
        else:
            todo = [o] if is_tagfile(data, o) else []
        for to in todo:
            tf = Tagfile(data, to)
            print("== section %d tagfile at %#x, Havok %s" % (si, to, tf.sdk))
            try:
                tf.type_bodies()
            except Exception as e:  # noqa: BLE001
                print("   (layouts incomplete: %s)" % e)
            for i, T in enumerate(tf.types):
                if not T or "members" not in T:
                    continue
                print("  %s : %s  size %s" % (tf.tname(i), tf.tname(T.get("parent")), T.get("size")))
                for name, f, off, ty in T["members"]:
                    print("      +%#04x %s %s" % (off, name, tf.tname(ty) if ty < len(tf.types) else ty))
            counts = {}
            for t, f, off, c in tf.items[1:]:
                counts[tf.tname(t)] = counts.get(tf.tname(t), 0) + 1
            print("  items:", counts)


def map_tiles(area, mapname, local_dir=None):
    """A map's static compound tiles: romfs paths from the dump's index, or files in local_dir (decompressed,
    named like the romfs files without .zs)."""
    stem = mapname + "_X"
    ok = lambda n: (n.startswith(stem) or n.startswith(mapname + ".")) and ".bphsc" in n  # noqa: E731
    if local_dir:
        return sorted(os.path.join(local_dir, n) for n in os.listdir(local_dir) if ok(n) and not n.endswith(".zs"))
    import gamefiles
    pre = "romfs/Phive/StaticCompoundBody/%s/" % area
    return sorted(p for p in gamefiles.nup().index if p.startswith(pre) and ok(p[len(pre):]) and p.endswith(".zs"))


SKIP_LAYERS_DEFAULT = ("GroundHighResRough",)


def map_triangles(paths, everything=False, log=print):
    """World-space triangles of the given tiles: (tiles, tris).

    tris: (v0, v1, v2, attrs), attrs = dict(layer, material, hitsPlayer, src). By default only what can stop
    the player: no sensors, no 'extra' (unplaced) bodies, no materials whose hit mask leaves out the Player layer
    (e.g. enemy-only air walls), and not GroundHighResRough (the detailed twin of GroundHighResSmooth - see the
    report). everything=True keeps all of it (attributes say what each triangle is).
    """
    tris, tiles = [], []
    for p in paths:
        sc = decode_bphsc(load(p))
        n0 = len(tris)
        dropped = {}

        def drop(why, n):
            dropped[why] = dropped.get(why, 0) + n
        for L in sc["layers"]:
            if L["extra"] and not everything:
                drop("extra bodies", sum(len(i["tris"]) for i in L["instances"]))
                continue
            for inst in L["instances"]:
                ln, sensor, mats, hits = instance_props(sc, inst)
                if not everything and (sensor or ln in SKIP_LAYERS_DEFAULT):
                    drop("sensor" if sensor else ln, len(inst["tris"]))
                    continue
                m0, mn = (inst["info"][7], inst["info"][8]) if inst.get("info") else (0, 0)
                for a, b, c, tag in inst["tris"]:
                    mi = m0 + tag if tag < mn else m0
                    mat = name_of(MATERIALS, sc["materials"][mi][0]) if mi < len(sc["materials"]) else "?"
                    hit = bool(sc["layermasks"][mi][0] & PLAYER_LAYER_BIT) if mi < len(sc["layermasks"]) else hits
                    if not everything and not hit:
                        drop("not hit by player (%s)" % mat, 1)
                        continue
                    tris.append((a, b, c, {"layer": ("sensor:" if sensor else "") + ln, "material": mat,
                                           "hitsPlayer": hit and not sensor, "src": "compound"}))
        g = sc["ground"]
        if g:
            for a, b, c, tag in g["tris"]:
                m = g["materials"][tag] if tag < len(g["materials"]) else (0, 0, 0)
                mask = g["layermasks"][tag][0] if tag < len(g["layermasks"]) else ~0
                tris.append((a, b, c, {"layer": "Ground", "material": name_of(MATERIALS, m[0]),
                                       "hitsPlayer": bool(mask & PLAYER_LAYER_BIT), "src": "ground"}))
        mine = tris[n0:]
        by_layer = {}
        for t in mine:
            by_layer[t[3]["layer"]] = by_layer.get(t[3]["layer"], 0) + 1
        tiles.append({"path": p, "name": sc["name"], "triangles": len(mine), "byLayer": by_layer,
                      "dropped": dropped, "bbox": bbox(mine), "notDecoded": sc["unknown"],
                      "otherSections": [[si, len(raw)] for si, raw in sc["other"]]})
        log("%s: %d triangles %s %s" % (sc["name"], len(mine), fmt_box(bbox(mine)), by_layer))
    return tiles, tris


def map_json(area, mapname, tiles, tris):
    """Compact JSON document: shared vertex list (mm-rounded) + triangles as index triples with attributes."""
    vidx, verts = {}, []
    names = {"class": [], "material": [], "layer": []}

    def code(kind, v):
        lst = names[kind]
        if v not in lst:
            lst.append(v)
        return lst.index(v)

    def vi(v):
        k = (round(v[0], 3), round(v[1], 3), round(v[2], 3))
        if k not in vidx:
            vidx[k] = len(verts)
            verts.append(list(k))
        return vidx[k]
    out = []
    for t in tris:
        out.append([vi(t[0]), vi(t[1]), vi(t[2]), code("class", classify(t)), code("material", t[3]["material"]),
                    code("layer", t[3]["layer"]), int(t[3]["hitsPlayer"])])
    return {"source": "game files (tools/phive.py)", "area": area, "map": mapname, "tiles": tiles,
            "triangleCount": len(tris), "bbox": bbox(tris), "vertices": verts,
            "triangleFields": ["v0", "v1", "v2", "class", "material", "layer", "hitsPlayer"],
            "classNames": names["class"], "materialNames": names["material"], "layerNames": names["layer"],
            "triangles": out}


def main():
    a = sys.argv[1:]
    everything = "--all" in a
    local_dir = None
    if "--dir" in a:
        i = a.index("--dir")
        local_dir = a[i + 1]
        del a[i:i + 2]
    a = [x for x in a if x != "--all"]
    if len(a) >= 3 and a[0] in ("map", "obj"):
        paths = map_tiles(a[1], a[2], local_dir)
        if not paths:
            raise SystemExit("no static compound tiles for %s/%s" % (a[1], a[2]))
        tiles, tris = map_triangles(paths, everything, log=lambda s: print(s, file=sys.stderr))
        if a[0] == "obj":
            with open(a[3], "w") as f:
                for t in tris:
                    for v in t[:3]:
                        f.write("v %.3f %.3f %.3f\n" % v)
                for i in range(len(tris)):
                    f.write("f %d %d %d\n" % (3 * i + 1, 3 * i + 2, 3 * i + 3))
            print("%d triangles -> %s" % (len(tris), a[3]), file=sys.stderr)
            return
        doc = map_json(a[1], a[2], tiles, tris)
        if len(a) > 3:
            with open(a[3], "w") as f:
                json.dump(doc, f, separators=(",", ":"))
            print("%d triangles, %d vertices -> %s" % (len(tris), len(doc["vertices"]), a[3]), file=sys.stderr)
        else:
            json.dump(doc, sys.stdout, separators=(",", ":"))
    elif len(a) == 2 and a[0] == "types":
        dump_types(a[1])
    elif len(a) == 1:
        summary(a[0])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
