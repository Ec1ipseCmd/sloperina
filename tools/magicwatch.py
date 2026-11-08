"""Catch the game code that writes a save field (default: Current magic) with Lichii's GDB stub.

Setup (once): Lichii > Emulation > Configure > Debug > "Enable GDB Stub" (port 6543), then start the game.
With the stub on, Lichii holds the game at boot until a debugger connects; this script connects,
lets it boot, waits for your save to load, then sets a write watchpoint on the field.

    python magicwatch.py                     watch Current magic; report in magicwatch_report.txt
    python magicwatch.py --field "Rupees"    any Int field title from field_hashes.json
    python magicwatch.py --dis 0x80123456    disassemble live game code (no GDB needed)
    python magicwatch.py --xref 0x80123456   find bl/b to an address in the main module (no GDB needed)

Addresses are guest addresses (Eden layout), the same ones game.py uses.
"""
import argparse
import json
import os
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import game  # noqa: E402
import win32  # noqa: E402

REPORT = os.path.join(HERE, "magicwatch_report.txt")


# ------------------------------------------------------------------ GDB remote protocol
class Rsp:
    def __init__(self, host, port, log):
        self.s = socket.create_connection((host, port), timeout=5)
        self.buf = b""
        self.ack = True
        self.log = log

    def _fill(self, timeout):
        self.s.settimeout(timeout)
        d = self.s.recv(65536)
        if not d:
            raise ConnectionError("stub closed the connection")
        self.buf += d

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        pkt = b"$" + data + b"#%02x" % (sum(data) & 0xFF)
        self.s.sendall(pkt)

    def recv(self, timeout=10.0):
        """Next packet's payload (str); None on timeout."""
        end = time.monotonic() + timeout
        while True:
            # Drop acks / stray bytes before the next packet start.
            i = self.buf.find(b"$")
            j = self.buf.find(b"%")
            starts = [x for x in (i, j) if x != -1]
            if starts:
                st = min(starts)
                h = self.buf.find(b"#", st)
                if h != -1 and len(self.buf) >= h + 3:
                    payload = self.buf[st + 1:h]
                    notification = self.buf[st:st + 1] == b"%"
                    self.buf = self.buf[h + 3:]
                    if self.ack and not notification:
                        self.s.sendall(b"+")
                    if notification:
                        continue
                    return self._unescape(payload).decode("latin-1")
            left = end - time.monotonic()
            if left <= 0:
                return None
            try:
                self._fill(left)
            except socket.timeout:
                return None

    @staticmethod
    def _unescape(p):
        out, i = bytearray(), 0
        while i < len(p):
            c = p[i]
            if c == 0x7D and i + 1 < len(p):
                out.append(p[i + 1] ^ 0x20)
                i += 2
            elif c == 0x2A and out and i + 1 < len(p):  # run-length: prev char repeated
                out += bytes([out[-1]]) * (p[i + 1] - 29)
                i += 2
            else:
                out.append(c)
                i += 1
        return bytes(out)

    def cmd(self, data, timeout=10.0):
        self.send(data)
        return self.recv(timeout)

    def interrupt(self, timeout=10.0):
        self.s.sendall(b"\x03")
        return self.recv(timeout)


def parse_stop(r):
    """'T05thread:1a;watch:2181..;' -> ('T05', {'thread': '1a', 'watch': ...})"""
    info = {}
    if r and r[0] == "T":
        for part in r[3:].split(";"):
            if ":" in part:
                k, v = part.split(":", 1)
                info[k] = v
    return (r or "")[:3], info


def regs_from_g(hexs):
    raw = bytes.fromhex(hexs)
    n = min(34, len(raw) // 8)
    vals = list(struct.unpack_from("<%dQ" % n, raw)) if n else []
    names = ["x%d" % i for i in range(29)] + ["fp", "lr", "sp", "pc", "cpsr"]
    r = dict(zip(names, vals))
    if "cpsr" in r:
        r["cpsr"] &= 0xFFFFFFFF
    return r


# ------------------------------------------------------------------ live game memory
def attach_game(log, need_save=True, field_hash=None):
    pid = None
    while not pid:
        pid = win32.find_pid("lichii.exe")
        if not pid:
            log("waiting for lichii.exe...")
            time.sleep(2)
    g = game.Game(win32.Process(pid))
    last = None
    while True:
        st = g.state
        if st != last:
            log("game: " + st)
            last = st
        if g.gb is not None and not need_save:
            return g
        if g.ready and (field_hash is None or field_hash in getattr(g, "int_by_hash", {})):
            return g
        time.sleep(0.5)


class Code:
    """Reads and disassembles guest code from Lichii's memory."""

    def __init__(self, g):
        import capstone
        self.g = g
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def where(self, a):
        M = self.g.M
        return f"main+{a - M:#x}" if M <= a < M + 0x6000000 else "?"

    def read(self, a, n):
        return self.g.p.read(self.g.gb + a, n)

    def dis(self, lo, hi, mark=None):
        lines = []
        try:
            data = self.read(lo, hi - lo)
        except OSError as e:
            return [f"  (can't read {lo:#x}: {e})"]
        for off in range(0, len(data) - 3, 4):
            pc = lo + off
            ins = next(self.md.disasm(data[off:off + 4], pc), None)
            text = f"{ins.mnemonic} {ins.op_str}" if ins else f".word {struct.unpack_from('<I', data, off)[0]:#010x}"
            m = "  <--" if pc == mark else ""
            lines.append(f"  {pc:#x} {self.where(pc):>14}: {text}{m}")
        return lines

    def func_start(self, pc, limit=0x2000):
        """Guess: scan back for the instruction after a ret / unconditional b, or a stp x29,x30 prologue."""
        lo = max(pc - limit, self.g.M)
        data = self.read(lo, pc - lo)
        for off in range(len(data) - 4, -1, -4):
            w = struct.unpack_from("<I", data, off)[0]
            if w in (0xD65F03C0, 0) or (w & 0xFC000000) == 0x14000000:  # ret / padding / b
                return lo + off + 4
        return None

    def xrefs(self, targets, span=game.DATA_OFF, data_span=0x2000000):
        """Direct b/bl in the code, plus 8-byte pointers (vtables, function tables) in the module's data."""
        import numpy as np
        M = self.g.M
        raw = b""
        for o in range(0, span, 8 << 20):
            try:
                raw += self.read(M + o, min(8 << 20, span - o))
            except OSError:
                break
        w = np.frombuffer(raw[:len(raw) // 4 * 4], dtype="<u4")
        pcs = M + np.arange(len(w), dtype=np.int64) * 4
        isbr = (w & 0x7C000000) == 0x14000000  # b / bl
        imm = (w & 0x03FFFFFF).astype(np.int64)
        imm = np.where(imm & 0x02000000, imm - 0x04000000, imm)
        dest = pcs + imm * 4
        data = b""
        for o in range(0, data_span, 1 << 20):
            try:
                data += self.read(M + span + o, 1 << 20)
            except OSError:
                break
        q = np.frombuffer(raw[:len(raw) // 8 * 8] + data[:len(data) // 8 * 8], dtype="<u8")
        qa = np.concatenate([M + np.arange(len(raw) // 8, dtype=np.int64) * 8,
                             M + span + np.arange(len(data) // 8, dtype=np.int64) * 8])
        out = {}
        for t in targets:
            hit = np.nonzero(isbr & (dest == t))[0]
            out[t] = [(int(pcs[i]), "bl" if w[i] & 0x80000000 else "b") for i in hit]
            out[t] += [(int(qa[i]), "ptr") for i in np.nonzero(q == t)[0]]
        return out


# ------------------------------------------------------------------ main job
def frames(code, regs, depth=12):
    """Return addresses via the frame-pointer chain (lr first)."""
    out = [regs.get("lr")]
    fp = regs.get("fp")
    for _ in range(depth):
        if not fp or fp & 7:
            break
        try:
            prev, ret = struct.unpack("<QQ", code.read(fp, 16))
        except OSError:
            break
        if not ret:
            break
        out.append(ret)
        if prev <= fp:
            break
        fp = prev
    return out


def watch(args, log):
    fields = json.load(open(os.path.join(os.path.dirname(HERE), "field_hashes.json"), encoding="utf-8"))
    h = next((int(k, 16) for k, v in fields.items() if v["title"] == args.field and v["type"] == "Int"), None)
    if h is None:
        raise SystemExit(f"no Int field titled {args.field!r} in field_hashes.json")

    log(f"connecting to Lichii's GDB stub on port {args.port} (enable it in Lichii's Debug settings)...")
    while True:
        try:
            rsp = Rsp("127.0.0.1", args.port, log)
            break
        except OSError:
            time.sleep(1)
    log("connected")
    sup = rsp.cmd("qSupported:multiprocess+;swbreak+;hwbreak+;xmlRegisters=aarch64") or ""
    if "QStartNoAckMode+" in sup and rsp.cmd("QStartNoAckMode") == "OK":
        rsp.ack = False
    rsp.cmd("?")
    if args.clear:  # watchpoints left by a run that was killed (they block reading those pages)
        rsp.interrupt()
        for a in args.clear:
            for x, n in ((a, 8), (a, 4), (a + 4, 4)):
                log(f"clear z2 {x:#x},{n}: {rsp.cmd(f'z2,{x:x},{n:x}')!r}")
    log("letting the game run (it was held at boot)")
    rsp.send("c")

    log(f"load your save; waiting for the {args.field!r} field...")
    g = attach_game(log, field_hash=h)
    code = Code(g)
    addr = g.int_base + g.int_by_hash[h] * game.INT_STRIDE + game.INT_VALUE
    log(f"{args.field}: guest {addr:#x} = {g.read_i32(addr)} / copy {g.read_i32(addr + 4)}  "
        f"(main {g.M:#x}, guestbase {g.gb:#x})")

    stop = rsp.interrupt()
    log(f"paused: {stop}")
    if args.queue:
        if args.bait is not None:
            rsp.cmd(f"M{addr:x},8:" + struct.pack("<ii", args.bait, args.bait).hex())
        bait = (lambda: rsp.cmd(f"M{addr:x},8:" + struct.pack("<ii", args.bait, args.bait).hex()))             if args.bait is not None else (lambda: None)
        return watch_queue(rsp, code, h, args, log, bait)
    wps = [(addr, 8)]
    if rsp.cmd(f"Z2,{addr:x},8") != "OK":
        wps = [(addr, 4), (addr + 4, 4)]
        for a, n in wps:
            r = rsp.cmd(f"Z2,{a:x},{n:x}")
            if r != "OK":
                rsp.cmd("D")
                raise SystemExit(f"stub refused the watchpoint at {a:#x}: {r!r}")
    log(f"write watchpoint set; running until {args.hits} writes or {args.timeout:.0f}s")

    def val():  # (watched pages can't be read from outside while the watchpoint is set)
        r = rsp.cmd(f"m{addr:x},8") or ""
        try:
            return struct.unpack("<ii", bytes.fromhex(r))
        except ValueError:
            return (None, r)

    def bait():  # the game only resets the value when it isn't already 0
        if args.bait is not None:
            rsp.cmd(f"M{addr:x},8:" + struct.pack("<ii", args.bait, args.bait).hex())

    hits, t_end = [], time.monotonic() + args.timeout
    while len(hits) < args.hits:
        bait()
        before = val()
        rsp.send("c")
        r = rsp.recv(max(0.1, t_end - time.monotonic()))
        if r is None:
            log("no more writes before the timeout")
            r = rsp.interrupt()
            break
        kind, info = parse_stop(r)
        th = info.get("thread")
        if th:
            rsp.cmd(f"Hg{th}")
        regs = regs_from_g(rsp.cmd("g") or "")
        # Step past the write with the watchpoint off, to see the new value.
        for a, n in wps:
            rsp.cmd(f"z2,{a:x},{n:x}")
        rsp.cmd(f"Hc{th}" if th else "Hc0")
        rsp.cmd("s", timeout=5)
        after = val()
        for a, n in wps:
            rsp.cmd(f"Z2,{a:x},{n:x}")
        hits.append({"stop": r, "thread": th, "regs": regs, "before": before, "after": after,
                     "frames": frames(code, regs)})
        log(f"hit {len(hits)}: pc {regs.get('pc', 0):#x} ({code.where(regs.get('pc', 0))})  "
            f"{before} -> {after}  [{r}]")

    for a, n in wps:
        rsp.cmd(f"z2,{a:x},{n:x}")
    rsp.cmd("D", timeout=3)
    log("watchpoint removed, debugger detached; the game keeps running")
    report(code, args.field, addr, hits, log)


def watch_queue(rsp, code, h, args, log, bait=lambda: None):
    """Watch GameData's Int command queue (12-byte {value, hash, op} slots) for commands on field h,
    to catch the code that posts them (the queue is applied later, by main+0xb99980)."""
    qbase, qlen = args.queue
    r = rsp.cmd(f"Z2,{qbase:x},{qlen:x}")
    if r != "OK":
        rsp.cmd("D")
        raise SystemExit(f"stub refused the queue watchpoint: {r!r}")
    log(f"watching queue {qbase:#x}+{qlen:#x} for commands on {h:#010x}")
    hits, total, t_end, pcs = [], 0, time.monotonic() + args.timeout, {}
    while len(hits) < args.hits and total < args.max_writes and time.monotonic() < t_end:
        if total % 20 == 0:
            bait()
        rsp.send("c")
        r = rsp.recv(max(0.1, t_end - time.monotonic()))
        if r is None:
            r = rsp.interrupt()
            break
        total += 1
        kind, info = parse_stop(r)
        th = info.get("thread")
        if th:
            rsp.cmd(f"Hg{th}")
        regs = regs_from_g(rsp.cmd("g") or "")
        rsp.cmd(f"z2,{qbase:x},{qlen:x}")
        rsp.cmd(f"Hc{th}" if th else "Hc0")
        rsp.cmd("s", timeout=5)
        w = int(info.get("watch", "0"), 16)
        k = (regs.get("pc"), regs.get("lr"))
        pcs[k] = pcs.get(k, 0) + 1
        slot = qbase + (w - qbase) // 12 * 12
        try:
            entry = struct.unpack("<III", bytes.fromhex(rsp.cmd(f"m{slot:x},c") or ""))
        except ValueError:
            entry = (None, None, None)
        rsp.cmd(f"Z2,{qbase:x},{qlen:x}")
        if entry[1] == h:
            hits.append({"stop": r, "thread": th, "regs": regs, "before": entry, "after": entry,
                         "frames": frames(code, regs)})
            log(f"match {len(hits)} (write {total}): pc {regs.get('pc', 0):#x} ({code.where(regs.get('pc', 0))}) "
                f"slot {slot:#x} = value {entry[0]}, op {entry[2]}")
    rsp.cmd(f"z2,{qbase:x},{qlen:x}")
    rsp.cmd("D", timeout=3)
    log(f"{total} queue writes seen; debugger detached")
    for (pc, lr), n in sorted(pcs.items(), key=lambda x: -x[1])[:15]:
        log(f"  {n:5} writes from pc {pc or 0:#x} ({code.where(pc or 0)}), lr {lr or 0:#x} ({code.where(lr or 0)})")
    report(code, f"queue commands for {h:#010x}", qbase, hits, log)


def report(code, field, addr, hits, log):
    out = [f"Writes to {field} (guest {addr:#x}); main = {code.g.M:#x}", ""]
    by_pc = {}
    for hgt in hits:
        by_pc.setdefault(hgt["regs"].get("pc"), []).append(hgt)
    for pc, hs in by_pc.items():
        h0 = hs[0]
        out.append(f"=== pc {pc:#x} ({code.where(pc or 0)}): {len(hs)} hit(s); values "
                   + ", ".join(f"{x['before'][0]}->{x['after'][0]}" for x in hs[:8]))
        out.append("registers: " + " ".join(f"{k}={v:#x}" for k, v in h0["regs"].items()))
        out.append("stop reply: " + h0["stop"])
        if pc:
            start = code.func_start(pc)
            out.append(f"function (guessed start {start:#x} {code.where(start)}):" if start else "function:")
            out += code.dis(max(start or pc - 0x80, pc - 0x200), pc + 0x40, mark=pc)
        out.append("call chain (return addresses):")
        for ret in h0["frames"]:
            if not ret:
                continue
            out.append(f"  -> {ret:#x} {code.where(ret)}")
            out += code.dis(ret - 0x28, ret + 0x4, mark=ret)
        out.append("")
    if not hits:
        out.append("No writes caught.")
    open(REPORT, "w", encoding="utf-8").write("\n".join(out))
    log(f"report: {REPORT}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--field", default="Current magic")
    ap.add_argument("--port", type=int, default=6543)
    ap.add_argument("--hits", type=int, default=12)
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds to wait for writes")
    ap.add_argument("--bait", type=int, default=240, help="value written before each run (default 240)")
    ap.add_argument("--queue", type=lambda x: int(x, 0), nargs=2, metavar=("ADDR", "LEN"),
                    help="watch GameData's Int command queue instead, for commands on the field")
    ap.add_argument("--max-writes", type=int, default=3000)
    ap.add_argument("--clear", type=lambda x: int(x, 0), nargs="+", metavar="ADDR", default=[],
                    help="remove watchpoints a killed run left at these addresses")
    ap.add_argument("--dis", type=lambda x: int(x, 0), nargs="+", metavar="ADDR")
    ap.add_argument("--xref", type=lambda x: int(x, 0), nargs="+", metavar="ADDR")
    args = ap.parse_args()
    log = lambda m: print(time.strftime("%H:%M:%S"), m, flush=True)  # noqa: E731

    if args.dis or args.xref:
        g = attach_game(log, need_save=False)
        code = Code(g)
        if args.dis:
            lo, hi = (args.dis[0] - 0x40, args.dis[0] + 0x40) if len(args.dis) == 1 else args.dis[:2]
            print("\n".join(code.dis(lo, hi, mark=args.dis[0] if len(args.dis) == 1 else None)))
        if args.xref:
            for t, refs in code.xrefs(args.xref).items():
                print(f"{t:#x} {code.where(t)}: {len(refs)} reference(s)")
                for pc, k in refs:
                    print(f"  {k:2} at {pc:#x} {code.where(pc)}")
        g.close()
        return
    watch(args, log)


if __name__ == "__main__":
    main()
