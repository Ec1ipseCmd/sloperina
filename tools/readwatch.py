"""Catch the game code that READS a Bool save field (e.g. a dungeon's IsGetMap), with Lichii's GDB stub.

Setup: Lichii > Emulation > Configure > Debug > "Enable GDB Stub" (port 6543). Start this script; it connects
and lets the game boot (the stub holds it at boot until a debugger connects).

    python readwatch.py <field name>     e.g. DungeonInfo.InsideTheDekuTree.IsGetMap
    (create tools/readwatch.arm)         set a read watchpoint on the field's value bytes, record readers

Report: tools/readwatch_report.txt. The watchpoint is removed and the debugger detaches at the end.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import magicwatch as mw  # noqa: E402
from gamedata import murmur3  # noqa: E402

ARM = os.path.join(HERE, "readwatch.arm")
REPORT = os.path.join(HERE, "readwatch_report.txt")
MAX_HITS, TIMEOUT = 60, 150.0


def log(m):
    line = time.strftime("%H:%M:%S ") + m
    print(line, flush=True)
    with open(REPORT + ".log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def main():
    field = sys.argv[1]
    h = murmur3(field)
    if os.path.exists(ARM):
        os.remove(ARM)
    log("connecting to Lichii's GDB stub on 6543...")
    while True:
        try:
            rsp = mw.Rsp("127.0.0.1", 6543, log)
            break
        except OSError:
            time.sleep(0.5)
    sup = rsp.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;xmlRegisters=aarch64") or ""
    if "QStartNoAckMode+" in sup and rsp.cmd("QStartNoAckMode") == "OK":
        rsp.ack = False
    rsp.cmd("?")
    log("connected; letting the game run")
    rsp.send("c")
    g = mw.attach_game(log)
    while not g._bools_ready():
        time.sleep(0.5)
    code = mw.Code(g)
    log(f"save loaded (main {g.M:#x}); waiting for {ARM}")
    while not os.path.exists(ARM):
        time.sleep(0.2)
    g._bools_ready()
    addr = g.bool_base + g.bool_by_hash[h] * g.BOOL_STRIDE + g.BOOL_VALUE
    log(f"{field} ({h:#x}) value at {addr:#x} = {g.get_bool(h)}; map {g.get_str('map')}")
    rsp.interrupt()
    r = rsp.cmd(f"Z3,{addr:x},2")
    log(f"read watchpoint: {r!r}")
    if r != "OK":
        r = rsp.cmd(f"Z4,{addr:x},2")
        log(f"access watchpoint instead: {r!r}")
    hits, t_end = [], time.monotonic() + TIMEOUT
    log("ARMED: open the map screen now")
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
        for z in ("z3", "z4"):
            rsp.cmd(f"{z},{addr:x},2")
        rsp.cmd(f"Hc{th}" if th else "Hc0")
        rsp.cmd("s", timeout=5)
        rsp.cmd(f"Z3,{addr:x},2")
        fr = mw.frames(code, regs)
        hits.append((regs, fr))
        log(f"hit {len(hits)}: pc {code.where(regs.get('pc', 0))} stack "
            + " < ".join(code.where(x or 0) for x in fr[:10]))
    for z in ("z3", "z4"):
        rsp.cmd(f"{z},{addr:x},2")
    # Lichii's stub leaves the game halted on detach: resume it, then just close the connection.
    rsp.send("c")
    time.sleep(0.3)
    rsp.s.close()
    log("watchpoint removed, game resumed, debugger disconnected")
    out, seen = [f"Reads of {field} at {addr:#x}, main {g.M:#x}", ""], set()
    for regs, fr in hits:
        key = (regs.get("pc"), tuple(fr[:6]))
        if key in seen:
            continue
        seen.add(key)
        out.append(f"pc {code.where(regs.get('pc', 0))}  x0..x3: "
                   + ", ".join(f"{regs.get(f'x{i}', 0):#x}" for i in range(4)))
        out += ["    " + code.where(x or 0) for x in fr]
    open(REPORT, "w", encoding="utf-8").write("\n".join(out) + "\n")
    log(f"report: {REPORT}")


if __name__ == "__main__":
    main()
