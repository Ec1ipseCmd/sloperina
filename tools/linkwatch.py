"""Find a meter on Link (e.g. the dive meter) by recording his objects while it runs down and refills.

    python linkwatch.py record [seconds] [out.npz]   default 150 s, tools/linkwatch.npz (read-only)
    python linkwatch.py find [out.npz]               values that drain and refill, best first
    python linkwatch.py meter                         dive meter source: snapshots at set meter levels

While recording: stand still at the surface a few seconds, dive and stay under until the meter has
dropped a good way, surface and let it refill; repeat once or twice. The recording covers Link's actor,
his status object ([[link+8]+0x6A0], where health lives) and every object either points to directly.
"""
import os
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import game  # noqa: E402
import win32  # noqa: E402

HEAP = (0x2000000000, 0x2400000000)
ROOT_SIZE, CHILD_SIZE = 0x4000, 0x800
RATE = 5.0  # samples per second


def regions(g):
    """[(guest address, size)]: Link's actor, his status object, and what they point to."""
    link = g.link_obj
    status = g.gq((g.gq(link + 8) or 0) + 0x6A0)
    roots = [(link, ROOT_SIZE)] + ([(status, ROOT_SIZE)] if status else [])
    seen = {a for a, _ in roots}
    out = list(roots)
    for base, size in roots:
        raw = g.p.read(g.gb + base, size)
        for (p,) in struct.iter_unpack("<Q", raw):
            if HEAP[0] <= p < HEAP[1] and p % 8 == 0 and p not in seen:
                try:
                    g.p.read(g.gb + p, CHILD_SIZE)
                except OSError:
                    continue
                seen.add(p)
                out.append((p, CHILD_SIZE))
    return out


def record(seconds, path):
    g = game.Game(win32.Process(win32.find_pid("lichii.exe")))
    t0 = time.monotonic()
    while not (g.ready and g.link_obj) and time.monotonic() - t0 < 15:
        time.sleep(0.1)
    regs = regions(g)
    total = sum(s for _, s in regs)
    print(f"{len(regs)} objects, {total / 1024:.0f} KiB per sample; recording {seconds:.0f} s at {RATE:.0f}/s")
    samples, times = [], []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        t = time.monotonic()
        buf = bytearray()
        for a, s in regs:
            try:
                buf += g.p.read(g.gb + a, s)
            except OSError:
                buf += bytes(s)
        samples.append(np.frombuffer(bytes(buf), dtype="<u4"))
        times.append(t)
        time.sleep(max(0.0, 1 / RATE - (time.monotonic() - t)))
    addrs = np.concatenate([np.arange(a, a + s, 4, dtype=np.uint64) for a, s in regs])
    np.savez_compressed(path, data=np.stack(samples), times=np.array(times), addrs=addrs,
                        link=g.link_obj)
    print(f"{len(samples)} samples -> {path}")


def find(path):
    z = np.load(path)
    raw, t, addrs = z["data"], z["times"], z["addrs"]
    print(f"{raw.shape[0]} samples over {t[-1] - t[0]:.0f} s, {raw.shape[1]} values; link actor {int(z['link']):#x}")
    results = []
    for kind, vals in (("f32", raw.view("<f4").astype(np.float64)), ("s32", raw.view("<i4").astype(np.float64))):
        with np.errstate(all="ignore"):
            ok = np.all(np.isfinite(vals), axis=0) & (np.abs(vals).max(axis=0) < 1e7)
        vals = np.where(ok, vals, 0)
        top, bottom = vals.max(axis=0), vals.min(axis=0)
        span = top - bottom
        d = np.diff(vals, axis=0)
        drops = (d < 0).sum(axis=0)       # samples where it went down
        rises = (d > 0).sum(axis=0)
        big_rise = (d > 0.3 * np.maximum(span, 1e-9)).sum(axis=0)  # refills come back fast
        at_top = (vals >= top - 0.02 * span).sum(axis=0)
        # a meter: a real range, rests full a good part of the time, drains over many samples
        # (more steps down than up), refills, and returns to the same full value
        score = np.where(ok & (span > 0) & (drops >= 10) & (at_top >= 5) & (drops > rises),
                         drops * np.minimum(at_top, drops) / (1 + rises - np.minimum(rises, big_rise * 3)), 0)
        for i in np.argsort(score)[::-1][:15]:
            if score[i] <= 0:
                break
            results.append((score[i], kind, int(addrs[i]), top[i], bottom[i], int(drops[i]), int(rises[i])))
    results.sort(reverse=True)
    link = int(z["link"])
    for sc, kind, a, top, bottom, dr, ri in results[:20]:
        rel = f"link+{a - link:#x}" if 0 <= a - link < ROOT_SIZE else ""
        print(f"{a:#x} {rel:14} {kind}  full {top:10.3f}  lowest {bottom:10.3f}  down {dr:4d} up {ri:4d}  score {sc:.0f}")


# ------------------------------------------------------------------ dive meter source
# The dive meter's readout ([link+0x6F8] +0x18C/+0x190/+0x194, f32 0..1, three copies) is recomputed
# every frame, so writing it does nothing. Its source changes in step with it: snapshot a wider graph
# (two pointers deep) at set readout levels during one dive and keep values that are an exact straight-line
# function of the readout.

METER_PTR, METER_OFF = 0x6F8, 0x18C
STEP, MAX_SNAPS = 0.01, 30


def wide_regions(g, cap=20000):
    link = g.link_obj
    o1 = g.gq(link + 8)
    status = g.gq((o1 or 0) + 0x6A0)
    meter = g.gq(link + METER_PTR)
    roots = [(a, ROOT_SIZE) for a in (link, o1, status, meter) if a]
    seen = {a for a, _ in roots}
    out, frontier = list(roots), list(roots)
    for _depth in range(2):
        nxt = []
        for base, size in frontier:
            try:
                raw = g.p.read(g.gb + base, size)
            except OSError:
                continue
            for (p,) in struct.iter_unpack("<Q", raw):
                if HEAP[0] <= p < HEAP[1] and p % 8 == 0 and p not in seen and len(seen) < cap:
                    seen.add(p)
                    nxt.append((p, CHILD_SIZE))
        out += nxt
        frontier = nxt
    good = []
    for a, s in out:
        try:
            g.p.read(g.gb + a, 8)
            good.append((a, s))
        except OSError:
            pass
    return good


def snapshot(g, regs):
    buf = bytearray()
    for a, s in regs:
        try:
            buf += g.p.read(g.gb + a, s)
        except OSError:
            buf += bytes(s)
    return np.frombuffer(bytes(buf), dtype="<u4")


def meter_source(timeout=300.0):
    g = game.Game(win32.Process(win32.find_pid("lichii.exe")))
    t0 = time.monotonic()
    # Only the emulator's memory and Link's actor are needed, not the save data (which a fresh attach
    # sometimes doesn't find after a warp).
    while not (g.gb and g.M and g._link_obj()) and time.monotonic() - t0 < 15:
        time.sleep(0.1)
    g.link_obj = g._link_obj()
    if not g.link_obj:
        sys.exit("Link not found")
    meter = g.gq(g.link_obj + METER_PTR)
    read = lambda: struct.unpack("<f", g.p.read(g.gb + meter + METER_OFF, 4))[0]  # noqa: E731
    regs = wide_regions(g)
    print(f"{len(regs)} objects, {sum(s for _, s in regs) / 2**20:.1f} MiB per snapshot", flush=True)
    print("waiting for the meter to be full, then for a long dive...", flush=True)
    # A snapshot whenever the meter has moved by STEP since the last one, over as many dives as it
    # takes (short dives only drain it a little), at most MAX_SNAPS, no more than 3 at full.
    snaps, levels = [], []
    end = time.monotonic() + timeout
    while len(snaps) < MAX_SNAPS and time.monotonic() < end:
        v = read()
        full = sum(1 for lv in levels if lv >= 0.999)
        if (not levels or min(abs(v - lv) for lv in levels) >= STEP) and (v < 0.999 or full < 3):
            t = time.monotonic()
            snaps.append(snapshot(g, regs))
            levels.append(read())
            print(f"snapshot {len(snaps)} at meter {levels[-1]:.3f} ({time.monotonic() - t:.2f} s)", flush=True)
        time.sleep(0.02)
    if len(snaps) < 6:
        print("not enough snapshots (dive longer?)")
        return
    data = np.stack(snaps)
    addrs = np.concatenate([np.arange(a, a + s, 4, dtype=np.uint64) for a, s in regs])
    lv = np.array(levels)
    x = lv - lv.mean()
    found = []
    for kind, vals in (("f32", data.view("<f4").astype(np.float64)), ("s32", data.view("<i4").astype(np.float64)),
                       ("u16lo", (data & 0xFFFF).astype(np.float64)), ("u16hi", (data >> 16).astype(np.float64))):
        with np.errstate(all="ignore"):
            ok = np.all(np.isfinite(vals), axis=0) & (np.ptp(vals, axis=0) > 1e-6) & (np.abs(vals).max(axis=0) < 1e8)
            vv = vals[:, ok] - vals[:, ok].mean(axis=0)
            r = (x @ vv) / (np.sqrt((x @ x) * (vv * vv).sum(axis=0)) + 1e-30)
        idx = np.nonzero(ok)[0]
        for j in np.nonzero(np.abs(r) > 0.999)[0]:
            i = idx[j]
            k = np.polyfit(lv, vals[:, i], 1)
            found.append((float(abs(r[j])), kind, int(addrs[i]), k, vals[:, i]))
    found.sort(key=lambda f: -f[0])
    link = g.link_obj
    print(f"{len(found)} values move in step with the meter (levels {np.round(lv, 3).tolist()}):")
    for absr, kind, a, k, v in found[:40]:
        print(f"{a:#x} {kind:5} r={absr:.5f} value = {k[0]:.4f} * meter + {k[1]:.4f}  "
              f"({' '.join(f'{x:.3f}' for x in v)})")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "record":
        record(float(a[1]) if len(a) > 1 else 150.0, a[2] if len(a) > 2 else os.path.join(HERE, "linkwatch.npz"))
    elif a and a[0] == "meter":
        meter_source(float(a[1]) if len(a) > 1 else 300.0)
    elif a and a[0] == "find":
        find(a[1] if len(a) > 1 else os.path.join(HERE, "linkwatch.npz"))
    else:
        raise SystemExit(__doc__)
