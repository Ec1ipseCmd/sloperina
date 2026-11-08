"""Record what a wall does to Link, read-only, while you play (safe alongside ootmenu).

    python wallwatch.py [seconds] [out.csv]      default 240 s, tools/wallwatch.csv

One row per change seen (polled ~every ms): the game's frame counter, Link's physics record position (what
ootmenu writes; 0.585 above the drawn position), the drawn position, the requested velocity and the velocity
after collision from the physics record (the difference is what the wall did), the actor's velocity (+0x388)
and momentum (+0x3A0). Velocities are units/s, (x, y, z). See docs/FINDINGS.md §5.10.
"""
import ctypes
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import game  # noqa: E402
import win32  # noqa: E402

COLS = ("t frame rec_x rec_y rec_z disp_x disp_y disp_z req_vx req_vy req_vz col_vx col_vy col_vz "
        "act_vx act_vy act_vz mom_x mom_y mom_z").split()


def menu_record():
    """The physics record ootmenu last moved Link with (from its log), or None. Several records follow
    Link; this is the one the levitate writes, so it's the one worth watching."""
    import re
    import tempfile
    try:
        text = open(os.path.join(tempfile.gettempdir(), "ootmenu.log")).read()
    except OSError:
        return None
    found = re.findall(r"moved Link with record (0x[0-9a-f]+)", text)
    return int(found[-1], 16) if found else None


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "wallwatch.csv")
    ctypes.WinDLL("winmm").timeBeginPeriod(1)
    g = game.Game(win32.Process(win32.find_pid("lichii.exe")))
    t0 = time.monotonic()
    while not (g.ready and g.link_obj) and time.monotonic() - t0 < 15:
        time.sleep(0.1)
    rec = None
    with open(out, "w") as f:
        f.write(",".join(COLS) + "\n")
        last = None
        end = time.monotonic() + secs
        n = 0
        while time.monotonic() < end:
            try:
                if not (rec and g._record_ok(rec)):
                    rec = menu_record()
                    if rec and g._record_ok(rec):
                        print(f"record {rec:#x} (ootmenu's)")
                        continue
                    rec = None
                    d = g.display_pos()
                    cands = g._scan_candidates(0x2160000000, 0x2168000000, d) if d else []  # read-only
                    if not cands:
                        time.sleep(0.2)
                        continue
                    rec = cands[0]
                    print(f"record {rec:#x}")
                b = g.p.read(g.gb + rec, 0x48)
                o = g.gb + g.link_obj
                act = struct.unpack("<3f", g.p.read(o + 0x388, 12))
                mom = struct.unpack("<3f", g.p.read(o + 0x3A0, 12))
                disp = g.display_pos() or (0.0, 0.0, 0.0)
                frame = g.frame_count() or 0
            except (OSError, TypeError):
                time.sleep(0.05)
                continue
            pos = struct.unpack_from("<3d", b, 0)
            rz, rx, ry = struct.unpack_from("<3f", b, 0x24)   # stored (z, x, y)
            cz, cx, cy = struct.unpack_from("<3f", b, 0x3C)
            row = (frame, *pos, *disp, rx, ry, rz, cx, cy, cz, *act, *mom)
            if row != last:
                f.write(f"{time.monotonic() - t0:.4f}," + ",".join(f"{v:.4f}" for v in row) + "\n")
                last = row
                n += 1
            time.sleep(0.001)
    print(f"{n} rows -> {out}")


if __name__ == "__main__":
    main()
