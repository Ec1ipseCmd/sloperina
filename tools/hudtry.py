"""Experiments for refreshing the HUD: have the patched game call a function on the PlayerStatus screen.

    python hudtry.py screen                 print the PlayerStatus screen object and its vtable
    python hudtry.py call <fn> [x1]         game calls main+fn(screen, x1) on its next frame
    python hudtry.py vcall <slot> [x1]      same, for the screen's own vtable slot
"""
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import magicwatch  # noqa: E402
import patch  # noqa: E402

DPAD_VTABLE = 0x4BE1068  # D-pad widget class; the PlayerStatus screen holds one at +0x330


def find_screen(g):
    rd = lambda a, n: g.p.read(g.gb + a, n)  # noqa: E731
    q = lambda a: struct.unpack("<Q", rd(a, 8))[0]  # noqa: E731
    uim = q(q(g.M + 0x4C7E098))
    cnt, arr = struct.unpack("<i", rd(uim + 8, 4))[0], q(uim + 0x10)
    for i in range(cnt):
        s = q(arr + 8 * i)
        try:
            w = q(s + 0x330) if s else 0
            if w and q(w) == g.M + DPAD_VTABLE:
                return s, q(s)
        except OSError:
            pass
    return None, None


def main():
    a = sys.argv[1:]
    g = magicwatch.attach_game(lambda *_: 0, field_hash=0x76B18200)
    if not patch.active(g):
        raise SystemExit("patch not active in the running game (restart the game)")
    s, vt = find_screen(g)
    if not s:
        raise SystemExit("PlayerStatus screen not found")
    if a[0] == "screen":
        print(f"screen {s:#x}, vtable main+{vt - g.M:#x}")
        for k in range(60):
            f = struct.unpack("<Q", g.p.read(g.gb + vt + 8 * k, 8))[0]
            if not (g.M <= f < g.M + 0x3C80000):
                break
            print(f"  [{k:2}] main+{f - g.M:#x}")
        return
    if a[0] == "call":
        fn = int(a[1], 0)
    else:
        fn = struct.unpack("<Q", g.p.read(g.gb + vt + 8 * int(a[1], 0), 8))[0] - g.M
    x1 = int(a[2], 0) if len(a) > 2 else 0
    patch.call_function(g, fn, s, x1)
    time.sleep(0.5)
    left = g.get_field(patch.MAILBOX)
    print(f"requested main+{fn:#x}(screen={s:#x}, {x1:#x}); mailbox now {left} ({'handled' if left == 0 else 'not handled yet: game paused?'})")


if __name__ == "__main__":
    main()
