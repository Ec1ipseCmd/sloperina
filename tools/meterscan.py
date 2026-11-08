"""Find the value behind a draining gauge by scanning the whole game heap while it drains (read-only).

    python meterscan.py [minutes]           default 10; results also go to tools/meterscan_report.txt
    python meterscan.py [minutes] resume    carry on from where the last run stopped (tools/meterscan_state.json)

Built for the dive meter: its readout ([link+0x6F8] +0x18C, f32 0..1) is recomputed every frame, so the real
value is elsewhere. The heap (~5 GiB) is read in batches; each batch is read twice ~1 s apart while the readout
is draining, and a value is kept if it changed exactly like the readout did:
  proportional  v = k * meter          (time or amount left)      vB / vA == rB / rA
  elapsed       v = k * (1 - meter)    (time or amount used)      vB / vA == (1 - rB) / (1 - rA)
as f32, s32 and u16. Survivors are then sampled repeatedly against the readout and fitted to a straight line.
Dive (and stay under) several times while it runs; it only works while the meter is going down.
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
BATCH = 8            # chunks of 64 MiB per batch
GAP = 1.0            # seconds between the two reads of a batch
TOL = 0.004          # relative tolerance on the ratio tests
REPORT = os.path.join(HERE, "meterscan_report.txt")
STATE = os.path.join(HERE, "meterscan_state.json")


def save_state(next_chunk, found):
    import json
    with open(STATE, "w") as f:
        json.dump({"next": next_chunk, "found": [[a, k, r] for (a, k, r) in found]}, f)


def load_state():
    import json
    with open(STATE) as f:
        d = json.load(f)
    return d["next"], {(a, k, r): None for a, k, r in d["found"]}


def log(msg):
    print(msg, flush=True)
    with open(REPORT, "a", encoding="utf-8") as f:
        f.write(msg + "\n")


def main(minutes, resume=False):
    open(REPORT, "a" if resume else "w").close()
    import ctypes
    ctypes.WinDLL("winmm").timeBeginPeriod(1)
    g = game.Game(win32.Process(win32.find_pid("lichii.exe")))
    t0 = time.monotonic()
    while not (g.gb and g.M and g._link_obj()) and time.monotonic() - t0 < 15:
        time.sleep(0.1)
    def meter():
        """The readout (0..1), found again from Link if the game reloaded; -1 when unavailable."""
        try:
            link = g._link_obj()
            m = g.gq(link + 0x6F8) if link else None
            return struct.unpack("<f", g.p.read(g.gb + m + 0x18C, 4))[0] if m else -1.0
        except (OSError, TypeError):
            return -1.0

    draining = lambda: 0.03 < meter() < 0.97  # noqa: E731

    chunks = [(ga, len(d)) for ga, d in g._guest_chunks(HEAP[0], HEAP[1], 64 << 20, writable=True)]
    log(f"{len(chunks)} chunks, {sum(n for _, n in chunks) / 2**30:.2f} GiB; dive now (only scans while the meter drains)")
    found = {}  # (addr, kind, relation) -> (vA, vB, rA, rB)
    i = 0
    if resume and os.path.exists(STATE):
        i, found = load_state()
        log(f"resuming at chunk {i} with {len(found)} candidates")
    end = time.monotonic() + minutes * 60
    while i < len(chunks) and time.monotonic() < end:
        if not draining():
            time.sleep(0.05)
            continue
        batch = chunks[i:i + BATCH]
        first = []
        for ga, n in batch:
            r0 = meter()
            try:
                d = g.p.read(g.gb + ga, n)
            except OSError:
                d = None
            first.append((d, (r0 + meter()) / 2))
        time.sleep(GAP)
        if not draining():
            continue  # surfaced mid-batch: redo it on the next dive
        for (ga, n), (dA, rA) in zip(batch, first):
            r0 = meter()
            try:
                dB = g.p.read(g.gb + ga, n)
            except OSError:
                continue
            rB = (r0 + meter()) / 2
            if dA is None or rB >= rA - 0.005:
                continue
            a = np.frombuffer(dA, "<u4")
            b = np.frombuffer(dB, "<u4")
            moved = np.nonzero(a != b)[0]
            if not len(moved):
                continue
            for kind, conv in (("f32", lambda x: x.view("<f4").astype(np.float64)),
                               ("s32", lambda x: x.view("<i4").astype(np.float64)),
                               ("u16", lambda x: (x & 0xFFFF).astype(np.float64))):
                va, vb = conv(a[moved]), conv(b[moved])
                with np.errstate(all="ignore"):
                    ok = np.isfinite(va) & np.isfinite(vb) & (np.abs(va) > 1e-6) & (np.abs(va) < 1e8)
                    q = vb / va
                for rel, want in (("left", rB / rA), ("used", (1 - rB) / (1 - rA))):
                    hit = ok & (np.abs(q - want) <= TOL * abs(want))
                    for j in np.nonzero(hit)[0]:
                        addr = ga + int(moved[j]) * 4
                        found[(addr, kind, rel)] = (va[j], vb[j], rA, rB)
        i += len(batch)
        save_state(i, found)
        log(f"batch {i}/{len(chunks)} done, meter {meter():.3f}, candidates so far {len(found)}")
    if i < len(chunks):
        log(f"stopped after {i}/{len(chunks)} chunks (time limit)")
    if not found:
        log("no candidates")
        return
    # verify: sample every candidate with the readout while it keeps draining, fit a line
    keys = list(found)
    log(f"verifying {len(keys)} candidates over the next dives...")
    samples, levels = [], []
    vend = time.monotonic() + 120
    while len(samples) < 40 and time.monotonic() < vend:
        if not draining():
            time.sleep(0.05)
            continue
        r0 = meter()
        row = []
        for addr, kind, _ in keys:
            raw = g.p.read(g.gb + addr, 4)
            row.append(struct.unpack("<f", raw)[0] if kind == "f32" else
                       struct.unpack("<i", raw)[0] if kind == "s32" else struct.unpack("<H", raw[:2])[0])
        samples.append(row)
        levels.append((r0 + meter()) / 2)
        time.sleep(0.15)
    S, L = np.array(samples, float), np.array(levels)
    if len(L) < 5:
        log("not enough draining time to verify; unverified candidates:")
        for k, v in list(found.items())[:50]:
            log(f"  {k[0]:#x} {k[1]} {k[2]}" + (f"  {v[0]:.4f} -> {v[1]:.4f} while meter {v[2]:.3f} -> {v[3]:.3f}"
                                                 if v else ""))
        return
    x = L - L.mean()
    out = []
    for c, (addr, kind, rel) in enumerate(keys):
        y = S[:, c]
        if np.ptp(y) <= 0:
            continue
        yy = y - y.mean()
        r = (x @ yy) / (np.sqrt((x @ x) * (yy @ yy)) + 1e-30)
        k = np.polyfit(L, y, 1)
        out.append((abs(r), addr, kind, rel, k))
    out.sort(reverse=True)
    log(f"{len(out)} still moving; best fits (value = a * meter + b):")
    for absr, addr, kind, rel, k in out[:30]:
        log(f"  {addr:#x} {kind} {rel:4}  r={absr:.5f}  a={k[0]:.4f} b={k[1]:.4f}  (full = {k[0] + k[1]:.4f}, empty = {k[1]:.4f})")


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else 10.0, resume="resume" in sys.argv[2:])
