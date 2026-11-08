"""Catch the game code that makes the HUD's D-pad appear (what pause/unpause runs), with Lichii's GDB stub.

Setup: Lichii > Emulation > Configure > Debug > "Enable GDB Stub" (port 6543). Start this script first, then the
game (the stub holds the game at boot until a debugger connects; this script lets it run).

    python dpadwatch.py            connect, let the game run, wait for the save, then wait for the arm file
    (create tools/dpadwatch.arm)   set write watchpoints on the D-pad widget and record writes + call stacks

Bytes watched (they changed when a pause redrew the D-pad, see docs/FINDINGS.md 5.2): widget +0x28 (4 bytes) and +0x188
(4 bytes). Report: tools/dpadwatch_report.txt. The watchpoints are removed and the debugger detaches at the end.
"""
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import magicwatch as mw  # noqa: E402
import hudtry  # noqa: E402

ARM = os.path.join(HERE, "dpadwatch.arm")
REPORT = os.path.join(HERE, "dpadwatch_report.txt")
WATCH = [(0x28, 4), (0x188, 4)]
MAX_HITS, TIMEOUT = 80, 120.0


def log(m):
    line = time.strftime("%H:%M:%S ") + m
    print(line, flush=True)
    with open(REPORT + ".log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def main():
    if os.path.exists(ARM):
        os.remove(ARM)
    log("connecting to Lichii's GDB stub on 6543...")
    while True:
        try:
            rsp = mw.Rsp("127.0.0.1", 6543, log)
            break
        except OSError:
            time.sleep(1)
    sup = rsp.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;xmlRegisters=aarch64") or ""
    if "QStartNoAckMode+" in sup and rsp.cmd("QStartNoAckMode") == "OK":
        rsp.ack = False
    rsp.cmd("?")
    log("connected; letting the game run")
    rsp.send("c")
    g = mw.attach_game(log)
    code = mw.Code(g)
    log(f"save loaded (main {g.M:#x}); waiting for {ARM}")
    while not os.path.exists(ARM):
        time.sleep(0.2)
    screen, _ = hudtry.find_screen(g)
    w = g.gq(screen + 0x330)
    log(f"PlayerStatus screen {screen:#x}, D-pad widget {w:#x}")
    rsp.interrupt()
    wps = []
    for off, n in WATCH:
        r = rsp.cmd(f"Z2,{w + off:x},{n:x}")
        log(f"watch {w + off:#x},{n}: {r!r}")
        if r == "OK":
            wps.append((w + off, n))
    hits, t_end = [], time.monotonic() + TIMEOUT
    log("ARMED: pause and unpause now")
    while len(hits) < MAX_HITS and time.monotonic() < t_end:
        rsp.send("c")
        r = rsp.recv(max(0.1, min(t_end - time.monotonic(), 10.0 if hits else 1e9)))
        if r is None:
            r = rsp.interrupt()
            break
        kind, info = mw.parse_stop(r)
        th = info.get("thread")
        if th:
            rsp.cmd(f"Hg{th}")
        regs = mw.regs_from_g(rsp.cmd("g") or "")
        wa = int(info.get("watch", "0") or "0", 16)
        before = rsp.cmd(f"m{w:x},200") or ""
        for a, n in wps:
            rsp.cmd(f"z2,{a:x},{n:x}")
        rsp.cmd(f"Hc{th}" if th else "Hc0")
        rsp.cmd("s", timeout=5)
        after = rsp.cmd(f"m{w:x},200") or ""
        for a, n in wps:
            rsp.cmd(f"Z2,{a:x},{n:x}")
        fr = mw.frames(code, regs)
        diff = []
        try:
            b, a2 = bytes.fromhex(before), bytes.fromhex(after)
            diff = [f"+{i:#x}:{b[i]:02x}->{a2[i]:02x}" for i in range(min(len(b), len(a2))) if b[i] != a2[i]]
        except ValueError:
            pass
        hits.append((regs, fr, wa, diff))
        log(f"hit {len(hits)}: pc {code.where(regs.get('pc', 0))} watch {wa - w:+#x} {' '.join(diff)} "
            f"stack " + " < ".join(code.where(x or 0) for x in fr[:8]))
    for a, n in wps:
        rsp.cmd(f"z2,{a:x},{n:x}")
    # Lichii's stub leaves the game halted on detach: resume it, then just close the connection.
    rsp.send("c")
    time.sleep(0.3)
    rsp.s.close()
    log("watchpoints removed, game resumed, debugger disconnected")
    out = [f"D-pad widget {w:#x}, screen {screen:#x}, main {g.M:#x}", ""]
    seen = set()
    for regs, fr, wa, diff in hits:
        key = (regs.get("pc"), tuple(fr[:6]))
        if key in seen:
            continue
        seen.add(key)
        out.append(f"pc {code.where(regs.get('pc', 0))} watch {wa - w:+#x} {' '.join(diff)}")
        out.append("  x0..x3: " + ", ".join(f"{regs.get(f'x{i}', 0):#x}" for i in range(4)))
        out += ["    " + code.where(x or 0) for x in fr]
    open(REPORT, "w", encoding="utf-8").write("\n".join(out) + "\n")
    log(f"report: {REPORT}")


if __name__ == "__main__":
    main()
