"""Air walls from airwalls.json: placement maths, Link-inside tests and screen projection.

A wall's world point for a local shape point p is  T + R (S * p),  with R = Rz Ry Rx from the layout's
Rotate (radians), as sead's makeR builds it. Shapes are boxes (centre + half extents) and cylinders.
See docs/FINDINGS.md §5.11.
"""
import json
import math
from pathlib import Path

PATH = Path(__file__).with_name("airwalls.json")
# Masks that stop Link (HitOnlyCharacter walls are for enemies and NPCs; sight/object ones don't block him).
PLAYER_MASKS = {"HitOnlyPlayer", "Default", None}


def rot(rx, ry, rz):
    """3x3 rotation, rows, Rz Ry Rx (sead makeR)."""
    sx, cx, sy, cy, sz, cz = math.sin(rx), math.cos(rx), math.sin(ry), math.cos(ry), math.sin(rz), math.cos(rz)
    return ((cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz),
            (cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz),
            (-sy, sx * cy, cx * cy))


def mul(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def mul_t(m, v):  # transpose (inverse rotation)
    return (m[0][0] * v[0] + m[1][0] * v[1] + m[2][0] * v[2],
            m[0][1] * v[0] + m[1][1] * v[1] + m[2][1] * v[2],
            m[0][2] * v[0] + m[1][2] * v[1] + m[2][2] * v[2])


class Wall:
    def __init__(self, d):
        self.actor, self.mask, self.part = d["actor"], d.get("mask"), d.get("part")
        self.t, self.s, self.shapes = tuple(d["t"]), tuple(d["s"]), d["shapes"]
        self.r = rot(*d["r"])
        self.blocks_link = self.mask in PLAYER_MASKS
        # World-space geometry, worked out once (walls don't move): corners, edges as index pairs and,
        # for boxes, faces as index quads with the box's centre (to tell which way a face points), so a
        # frame projects each corner once.
        self.points, self.lines, self.faces = [], [], []  # faces: (quad, box centre)
        self.line_faces = []  # per edge: indices into faces of the faces it borders
        for sh in self.shapes:
            base = len(self.points)
            if sh["type"] == "box":
                c, h = sh["center"], sh["half"]
                for i in range(8):
                    self.points.append(self.world(tuple(c[k] + (h[k] if i >> k & 1 else -h[k])
                                                        for k in range(3))))
                centre = tuple(sum(p[k] for p in self.points[base:]) / 8 for k in range(3))
                f0 = len(self.faces)
                for k in range(3):
                    a, b = [j for j in range(3) if j != k]
                    for side in (0, 1):
                        quad = [base + ((side << k) | (ua << a) | (ub << b))
                                for ua, ub in ((0, 0), (1, 0), (1, 1), (0, 1))]
                        self.faces.append((quad, centre))
                for i in range(8):
                    for k in range(3):
                        if not i >> k & 1:
                            e = (base + i, base + (i | 1 << k))
                            self.lines.append(e)
                            self.line_faces.append([f0 + fi for fi, (q, _) in enumerate(self.faces[f0:])
                                                    if e[0] in q and e[1] in q])
            else:
                index = {}
                for a, b in self._shape_edges(sh):
                    ia = index.setdefault(a, len(index))
                    ib = index.setdefault(b, len(index))
                    self.lines.append((base + ia, base + ib))
                    self.line_faces.append([])
                self.points += list(index)
        pts = self.points
        self.centre = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3)) if pts else self.t
        self.radius = max((math.dist(p, self.centre) for p in pts), default=0.0)

    def world(self, p):
        q = mul(self.r, (p[0] * self.s[0], p[1] * self.s[1], p[2] * self.s[2]))
        return (q[0] + self.t[0], q[1] + self.t[1], q[2] + self.t[2])

    def edges(self):
        """World-space line segments outlining every shape."""
        return [(self.points[a], self.points[b]) for a, b in self.lines]

    def _shape_edges(self, sh):
        """World-space segments for a non-box shape (cylinder: two rings and four sides)."""
        out = []
        if sh["type"] == "cylinder":
            a, b, r = sh["a"], sh["b"], sh["radius"]
            n = 16
            ring = lambda c: [self.world((c[0] + r * math.cos(2 * math.pi * i / n), c[1],  # noqa: E731
                                          c[2] + r * math.sin(2 * math.pi * i / n))) for i in range(n)]
            ra, rb = ring(a), ring(b)
            out += [(ra[i], ra[(i + 1) % n]) for i in range(n)] + [(rb[i], rb[(i + 1) % n]) for i in range(n)]
            out += [(ra[i], rb[i]) for i in range(0, n, 4)]
        return out

    def distance(self, p):
        """Distance from world point p to the nearest point of the wall (0 inside it)."""
        q = mul_t(self.r, (p[0] - self.t[0], p[1] - self.t[1], p[2] - self.t[2]))  # scaled local
        best = math.inf
        for sh in self.shapes:
            if sh["type"] == "box":
                c, h = sh["center"], sh["half"]
                d = [max(abs(q[k] - c[k] * self.s[k]) - h[k] * abs(self.s[k]), 0.0) for k in range(3)]
                best = min(best, math.sqrt(sum(v * v for v in d)))
            else:
                best = min(best, max(math.dist(p, self.centre) - self.radius, 0.0))
        return best

    def inside(self, p):
        """For a world point inside a box shape of this wall: (depths, normals) per face, in world units
        and world directions (outward), nearest first; else None. Cylinders: radial and end faces."""
        q = mul_t(self.r, (p[0] - self.t[0], p[1] - self.t[1], p[2] - self.t[2]))
        for sh in self.shapes:
            if sh["type"] == "box":
                c, h = sh["center"], sh["half"]
                faces = []
                for k in range(3):
                    s = abs(self.s[k]) or 1e-9
                    d = q[k] / s - c[k]  # unscaled local
                    if abs(d) >= h[k]:
                        break
                    for sign in (1, -1):
                        depth = (h[k] - sign * d) * s
                        axis = [0.0, 0.0, 0.0]
                        axis[k] = sign
                        faces.append((depth, mul(self.r, tuple(axis))))
                else:
                    return sorted(faces, key=lambda f: f[0])
            elif sh["type"] == "cylinder":
                a, b, r = sh["a"], sh["b"], sh["radius"]
                lo, hi = min(a[1], b[1]), max(a[1], b[1])
                ly = q[1] / (self.s[1] or 1e-9)
                dx, dz = q[0] / (self.s[0] or 1e-9) - a[0], q[2] / (self.s[2] or 1e-9) - a[2]
                rr = math.hypot(dx, dz)
                if lo < ly < hi and rr < r:
                    out_dir = mul(self.r, (dx / rr, 0.0, dz / rr)) if rr > 1e-6 else mul(self.r, (1.0, 0.0, 0.0))
                    sxz = (abs(self.s[0]) + abs(self.s[2])) / 2
                    return sorted([((r - rr) * sxz, out_dir),
                                   ((hi - ly) * abs(self.s[1]), mul(self.r, (0, 1, 0))),
                                   ((ly - lo) * abs(self.s[1]), mul(self.r, (0, -1, 0)))], key=lambda f: f[0])
        return None


_data = None


def walls_for(mapname):
    """Walls placed in a map (empty if unknown or airwalls.json is missing)."""
    global _data
    if _data is None:
        try:
            _data = json.loads(PATH.read_text())["maps"]
        except (OSError, ValueError, KeyError):
            _data = {}
    return [Wall(d) for d in _data.get(mapname, [])]


class Camera:
    """Pinhole camera from the game's camera: position, look-at, vertical fov, aspect."""

    def __init__(self, pos, look, fov_y, aspect):
        self.p = pos
        f = [look[i] - pos[i] for i in range(3)]
        n = math.sqrt(sum(v * v for v in f)) or 1.0
        self.f = [v / n for v in f]
        r = (-self.f[2], 0.0, self.f[0])  # f x up(0,1,0)
        n = math.hypot(r[0], r[2]) or 1.0
        self.r = (r[0] / n, 0.0, r[2] / n)
        self.u = (self.r[1] * self.f[2] - self.r[2] * self.f[1],
                  self.r[2] * self.f[0] - self.r[0] * self.f[2],
                  self.r[0] * self.f[1] - self.r[1] * self.f[0])
        self.ty = math.tan(fov_y / 2)
        self.tx = self.ty * aspect

    def view(self, x):
        d = (x[0] - self.p[0], x[1] - self.p[1], x[2] - self.p[2])
        return (sum(d[i] * self.r[i] for i in range(3)), sum(d[i] * self.u[i] for i in range(3)),
                sum(d[i] * self.f[i] for i in range(3)))

    def screen(self, v, w, h):
        """Pixels of a view-space point (z > 0)."""
        return ((v[0] / (v[2] * self.tx) + 1) * w / 2, (1 - v[1] / (v[2] * self.ty)) * h / 2)

    def segment(self, a, b, w, h, near=0.05):
        """Screen pixels (x0, y0, x1, y1) of a world segment, clipped to the near plane and the view
        (with a margin); None if none of it shows."""
        return self.segment_view(self.view(a), self.view(b), w, h, near)

    def segment_view(self, va, vb, w, h, near=0.05):
        """segment() for points already in view space (see view())."""
        if va[2] < near and vb[2] < near:
            return None
        if va[2] < near or vb[2] < near:
            t = (near - va[2]) / (vb[2] - va[2])
            cut = tuple(va[i] + t * (vb[i] - va[i]) for i in range(3))
            va, vb = (cut, vb) if va[2] < near else (va, cut)
        pts = clip_rect([self.screen(va, w, h), self.screen(vb, w, h)], w, h, closed=False)
        return (*pts[0], *pts[1]) if len(pts) == 2 else None

    def polygon_view(self, vs, w, h, near=0.05):
        """Screen polygon [(x, y), ...] of a view-space polygon, clipped to the near plane and the view."""
        out = []
        for i, a in enumerate(vs):  # Sutherland-Hodgman against z = near
            b = vs[(i + 1) % len(vs)]
            if a[2] >= near:
                out.append(a)
            if (a[2] >= near) != (b[2] >= near):
                t = (near - a[2]) / (b[2] - a[2])
                out.append(tuple(a[k] + t * (b[k] - a[k]) for k in range(3)))
        if len(out) < 3:
            return []
        return clip_rect([self.screen(v, w, h) for v in out], w, h)


def clip_rect(pts, w, h, margin=16, closed=True):
    """Clip a screen polygon (or a 2-point segment, closed=False) to the view plus a margin, so Tk
    never gets huge coordinates and shapes keep their real edges at the screen border."""
    for axis, lim, keep_above in ((0, -margin, True), (0, w + margin, False),
                                  (1, -margin, True), (1, h + margin, False)):
        inside = (lambda p: p[axis] >= lim) if keep_above else (lambda p: p[axis] <= lim)  # noqa: E731
        out = []
        n = len(pts)
        for i in range(n if closed else n - 1):
            a, b = pts[i], pts[(i + 1) % n]
            if inside(a):
                out.append(a)
            if inside(a) != inside(b):
                t = (lim - a[axis]) / (b[axis] - a[axis])
                out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
        if not closed and pts and inside(pts[-1]):
            out.append(pts[-1])
        if not closed:  # a segment: its two surviving ends
            out = [out[0], out[-1]] if len(out) >= 2 else []
        pts = out
        if not pts:
            return []
    return pts


# Link's size for hiding walls behind him: (height, radius) in units, by age (from the map name).
LINK_SIZE = {"child": (1.15, 0.42), "adult": (1.75, 0.5)}


class Painter:
    """Draws walls as see-through solids on two canvases: faces on one (its window is very transparent)
    and outlines on the other (more opaque), back edges dashed. Walls further away than Link have his
    silhouette cut out of them (filled with the see-through key colour), so he stays in front of what's
    behind him. Canvas items are reused between frames (moving an item is much cheaper than deleting
    and creating it)."""

    FACE_LAYERS = ("far_face", "mask", "near_face")
    LINE_LAYERS = ("far_back_line", "far_front_line", "mask", "near_back_line", "near_front_line")

    def __init__(self, face_canvas, line_canvas, key):
        self.canvases = {"face": face_canvas, "line": line_canvas}
        self.key = key
        self.pool = {}  # (canvas, layer) -> [[item id, look it has or None]]
        self.masks = {k: cv.create_polygon(0, 0, 0, 0, 0, 0, fill=key, outline=key, tags=("mask",),
                                           state="hidden") for k, cv in self.canvases.items()}

    def _item(self, which, layer, n, coords, look):
        cv = self.canvases[which]
        pool = self.pool.setdefault((which, layer), [])
        opts = look_options(layer, look)
        if n < len(pool):
            item, had = pool[n]
            cv.coords(item, *coords)
            if had != look:
                cv.itemconfigure(item, state="normal", **opts)
                pool[n][1] = look
        else:
            make = cv.create_polygon if which == "face" else cv.create_line
            pool.append([make(*coords, tags=(layer,), **opts), look])

    def draw(self, cam, walls, w, h, styles, max_range, link=None):
        """styles: per wall, (color, width) or None to skip it. link: (feet position, height, radius)
        to keep Link in front of walls further away than him, or None."""
        count = {}
        link_dist = None
        if link:
            f, height, _ = link
            link_dist = math.dist(cam.p, (f[0], f[1] + height / 2, f[2]))
        order = sorted(((math.dist(wall.centre, cam.p), wall, st) for wall, st in zip(walls, styles)
                        if st is not None), key=lambda x: -x[0])  # far to near
        any_far = False
        for dist, wall, style in order:
            if dist - wall.radius > max_range:
                continue
            group = "far" if link_dist is not None and wall.distance(cam.p) > link_dist else "near"
            view = [cam.view(p) for p in wall.points]
            front = []
            for quad, centre in wall.faces:
                fc = [sum(wall.points[i][k] for i in quad) / 4 for k in range(3)]
                normal = [fc[k] - centre[k] for k in range(3)]
                front.append(sum(normal[k] * (cam.p[k] - fc[k]) for k in range(3)) > 0)
                if not front[-1]:
                    continue  # a box is convex: its front faces cover the back ones
                poly = cam.polygon_view([view[i] for i in quad], w, h)
                if len(poly) >= 3:
                    layer = f"{group}_face"
                    self._item("face", layer, count.get(layer, 0), [c for p in poly for c in p], style)
                    count[layer] = count.get(layer, 0) + 1
                    any_far |= group == "far"
            for (ia, ib), faces in zip(wall.lines, wall.line_faces):
                seg = cam.segment_view(view[ia], view[ib], w, h)
                if seg:
                    side = "front" if (not faces or any(front[f] for f in faces)) else "back"
                    layer = f"{group}_{side}_line"
                    self._item("line", layer, count.get(layer, 0), seg, style)
                    count[layer] = count.get(layer, 0) + 1
                    any_far |= group == "far"
        outline = self.link_outline(cam, link, w, h) if (link and any_far) else []
        for which, cv in self.canvases.items():
            m = self.masks[which]
            if len(outline) >= 3:
                cv.coords(m, *[c for p in outline for c in p])
                cv.itemconfigure(m, state="normal")
            else:
                cv.itemconfigure(m, state="hidden")
        for (which, layer), pool in self.pool.items():  # park what this frame didn't use
            for entry in pool[count.get(layer, 0):]:
                if entry[1] is not None:
                    self.canvases[which].itemconfigure(entry[0], state="hidden")
                    entry[1] = None
        for which, layers in (("face", self.FACE_LAYERS), ("line", self.LINE_LAYERS)):
            for layer in layers[1:]:  # stack back to front
                self.canvases[which].tag_raise(layer)
        return sum(count.values())

    @staticmethod
    def link_outline(cam, link, w, h):
        """Screen outline of Link: a capsule from his feet to his head, facing the camera."""
        f, height, r = link
        right = cam.r
        at = lambda y, side, lift=0.0: (f[0] + right[0] * side, f[1] + y + lift,  # noqa: E731
                                        f[2] + right[2] * side)
        pts = [at(0, -r * 0.7, -0.05), at(height * 0.35, -r), at(height, -r * 0.8), at(height, 0, r * 0.15),
               at(height, r * 0.8), at(height * 0.35, r), at(0, r * 0.7, -0.05)]
        return cam.polygon_view([cam.view(p) for p in pts], w, h)


def look_options(layer, look):
    """Tk options for an item of this layer in this (color, width)."""
    color, width = look
    if layer.endswith("face"):
        return {"fill": color, "outline": ""}
    if layer.endswith("back_line"):
        return {"fill": color, "width": 1, "dash": (4, 4)}
    return {"fill": color, "width": width}
