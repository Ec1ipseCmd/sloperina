"""List and extract files from the game's .nup dump (layout as documented by Hookshot's nx2/nup.py).

    python nupfs.py ls [pattern]            romfs/exefs paths containing pattern, with sizes
    python nupfs.py get <path> <out file>   extract one file (package-level zstd removed)
"""
import io
import struct
import sys
from pathlib import Path

import os

# The dump: $OOTMENU_NUP, else the first .nup two folders up from tools/.
NUP = os.environ.get("OOTMENU_NUP") or next(Path(__file__).resolve().parents[2].glob("*.nup"), None)
ZSTD = b"\x28\xb5\x2f\xfd"


class Nup:
    def __init__(self, path=NUP):
        self.f = open(path, "rb")
        h = self.f.read(0x80 + 0x30)
        assert h[:4] == b"NUP\x01" and h[0x80:0x84] == b"TOC\x01", "not a NUP package"
        nt, nd, nf, soff, ssz = struct.unpack_from("<IQQQQ", h, 0x84)
        dirs_at = 0x80 + 0x30 + 0x40 * nt
        files_at = dirs_at + 24 * nd
        kids_at = files_at + 80 * nf
        strings_at = 0x80 + soff
        self.f.seek(0)
        meta = self.f.read(strings_at + ssz)
        self.s = meta[strings_at:]
        self.nd = nd
        self.titles = [struct.unpack_from("<QIIII", meta, 0xB0 + 0x40 * i) for i in range(nt)]
        self.dirs = [struct.unpack_from("<4I", meta, dirs_at + 24 * i) for i in range(nd)]
        self.files = [struct.unpack_from("<IIQQ", meta, files_at + 80 * i) for i in range(nf)]
        self.kids = struct.unpack_from("<%dI" % ((strings_at - kids_at) // 4), meta, kids_at)

    def name(self, o, n):
        return self.s[o:o + n].decode("utf-8")

    def walk(self, d=None, prefix=""):
        if d is None:  # newest title: exefs + romfs
            t = max(self.titles, key=lambda t: t[4])
            yield from self.walk(t[2], "exefs/")
            yield from self.walk(t[3], "romfs/")
            return
        _, _, first, n = self.dirs[d]
        for k in self.kids[first:first + n]:
            if k < self.nd:
                yield from self.walk(k, prefix + self.name(*self.dirs[k][:2]) + "/")
            else:
                r = self.files[k - self.nd]
                yield prefix + self.name(r[0], r[1]), r

    def read(self, r):
        _, _, size, off = r
        self.f.seek(off)
        head = self.f.read(min(size, 1 << 16) + 18)
        if head[:4] == ZSTD:
            import zstandard
            self.f.seek(off)
            d = zstandard.ZstdDecompressor().decompressobj()
            out, left = io.BytesIO(), size + (1 << 16)
            try:
                while left and not d.eof and out.tell() < size:
                    b = self.f.read(min(left, 8 << 20))
                    if not b:
                        break
                    left -= len(b)
                    out.write(d.decompress(b))
                data = out.getvalue()
                if len(data) == size:
                    return data
            except zstandard.ZstdError:
                pass
        self.f.seek(off)
        return self.f.read(size)


def main():
    a = sys.argv[1:]
    n = Nup()
    if a and a[0] == "ls":
        pat = a[1].lower() if len(a) > 1 else ""
        for p, r in n.walk():
            if pat in p.lower():
                print(f"{r[2]:>12}  {p}")
    elif a and a[0] == "get":
        r = next(r for p, r in n.walk() if p == a[1])
        Path(a[2]).write_bytes(n.read(r))
        print("wrote", a[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
