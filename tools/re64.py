"""Static analysis helpers over a dump of the game's main module (main.bin + main.base).

    python re64.py <dir> xref <offset> [...]     code that loads main+offset (ADRP+ADD/LDR) or calls it (BL/B)
    python re64.py <dir> dis <offset> [count]     disassemble from main+offset; names hash constants
    python re64.py <dir> func <offset>            disassemble the whole function containing main+offset
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

D = Path(sys.argv[1])
BIN = (D / "main.bin").read_bytes()
TEXT_END = 0x3C80000  # code ends before rodata (first strings ~0x3c8xxxx)
W = np.frombuffer(BIN[:TEXT_END], dtype="<u4")
NAMES = {}
for p in (Path(__file__).resolve().parents[1] / "data" / "rom_names.json",
          Path(__file__).resolve().parents[1] / "data" / "memory_names.json"):  # later files win
    if p.exists():
        NAMES.update({int(h, 16): v[0] for h, v in json.loads(p.read_text(encoding="utf-8")).items()})


def xref(targets):
    pcs = np.arange(len(W), dtype=np.int64) * 4
    isadrp = (W & 0x9F000000) == 0x90000000
    immlo = (W >> 29) & 3
    immhi = (W >> 5) & 0x7FFFF
    imm = ((immhi.astype(np.int64) << 2) | immlo)
    imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
    page = (pcs & ~0xFFF) + (imm << 12)
    rd = W & 31
    out = {t: [] for t in targets}
    idx = np.nonzero(isadrp)[0]
    tpages = {t & ~0xFFF: t for t in targets}
    for i in idx:
        pg = int(page[i])
        if pg not in tpages and not any((t & ~0xFFF) == pg for t in targets):
            continue
        r = int(rd[i])
        for j in range(i + 1, min(i + 8, len(W))):  # the add/ldr using this register
            w = int(W[j])
            if (w & 0xFFC00000) == 0x91000000 and (w >> 5) & 31 == r:  # ADD Xd, Xn, #imm
                tgt = pg + ((w >> 10) & 0xFFF)
            elif (w & 0xFFC00000) == 0xF9400000 and (w >> 5) & 31 == r:  # LDR Xt, [Xn, #imm*8]
                tgt = pg + ((w >> 10) & 0xFFF) * 8
            else:
                continue
            if tgt in out:
                out[tgt].append(i * 4)
            break
    isbl = (W & 0x7C000000) == 0x14000000
    bimm = (W & 0x03FFFFFF).astype(np.int64)
    bimm = np.where(bimm & 0x02000000, bimm - 0x04000000, bimm)
    dest = pcs + bimm * 4
    for t in targets:
        out[t] += [int(p) for p in pcs[np.nonzero(isbl & (dest == t))[0]]]
    return out


def func_start(off):
    for o in range(off, max(off - 0x20000, 0), -4):
        w = struct.unpack_from("<I", BIN, o)[0]
        if w == 0xD503237F:  # pacibsp
            return o
        if w in (0xD65F03C0, 0xD65F0FFF, 0) and o != off:
            return o + 4
    return off


def dis(off, n):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    regs = {}
    for ins in md.disasm(BIN[off:off + 4 * n], off):
        note = ""
        op = ins.op_str
        if ins.mnemonic in ("mov", "movz", "movk") and "#" in op:
            r = op.split(",")[0].strip().replace("x", "w")
            try:
                v = int(op.split("#")[1].split(",")[0], 0)
            except ValueError:
                v = None
            if v is not None:
                if ins.mnemonic == "movk" and "lsl #16" in op:
                    v = (regs.get(r, 0) & 0xFFFF) | (v << 16)
                regs[r] = v & 0xFFFFFFFF
                if regs[r] in NAMES:
                    note = f"   ; {NAMES[regs[r]]}"
        if ins.mnemonic == "adrp":
            pass
        if ins.mnemonic in ("bl", "b"):
            note = ""
        print(f"  {ins.address:#09x}: {ins.mnemonic:<6} {op}{note}")
        if ins.mnemonic in ("ret", "retab") and n > 4000:
            break


def string_at(off):
    e = BIN.index(b"\0", off)
    return BIN[off:e].decode("latin-1")


def main():
    cmd = sys.argv[2]
    if cmd == "xref":
        ts = [int(x, 0) for x in sys.argv[3:]]
        for t, refs in xref(ts).items():
            print(f"{t:#x} {string_at(t)[:50]!r}: {len(refs)} ref(s)")
            for r in refs:
                print(f"   {r:#x}  (function {func_start(r):#x})")
    elif cmd == "dis":
        dis(int(sys.argv[3], 0), int(sys.argv[4]) if len(sys.argv) > 4 else 64)
    elif cmd == "func":
        s = func_start(int(sys.argv[3], 0))
        print(f"function at {s:#x}")
        dis(s, 5000)


if __name__ == "__main__":
    main()
