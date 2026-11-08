"""Minimal BYML (v2-v7, either byte order) reader: byml.load(bytes) -> Python objects."""
import struct


class Byml:
    def __init__(self, b):
        assert b[:2] in (b"BY", b"YB"), "not BYML"
        self.e = ">" if b[:2] == b"BY" else "<"
        self.bo = "big" if self.e == ">" else "little"
        self.b = b
        self.ver, ko, so, ro = struct.unpack_from(self.e + "HIII", b, 2)
        self.keys = self._strtab(ko) if ko else []
        self.strs = self._strtab(so) if so else []
        self.root = self.node(b[ro], ro) if ro else None

    def _strtab(self, o):
        b = self.b
        assert b[o] == 0xC2
        n = int.from_bytes(b[o + 1:o + 4], self.bo)
        offs = struct.unpack_from(self.e + "%dI" % (n + 1), b, o + 4)
        return [b[o + offs[i]:b.index(b"\0", o + offs[i])].decode("utf-8", "replace") for i in range(n)]

    def value(self, t, raw, at):
        b = self.b
        if t == 0xA0:
            return self.strs[raw]
        if t in (0xC0, 0xC1, 0x20, 0x21):
            return self.node(t, raw)
        if t == 0xA1:  # binary: u32 size + data
            n = struct.unpack_from(self.e + "I", b, raw)[0]
            return b[raw + 4:raw + 4 + n]
        if t == 0xD0:
            return bool(raw)
        if t == 0xD1:
            return struct.unpack("<i", struct.pack("<I", raw))[0]
        if t == 0xD2:
            return struct.unpack("<f", struct.pack("<I", raw))[0]
        if t == 0xD3:
            return raw
        if t in (0xD4, 0xD5, 0xD6):
            return struct.unpack_from(self.e + {0xD4: "q", 0xD5: "Q", 0xD6: "d"}[t], b, raw)[0]
        if t == 0xFF:
            return None
        raise ValueError(f"unknown BYML type {t:#x} at {at:#x}")

    def node(self, t, o):
        b = self.b
        assert b[o] == t, (hex(b[o]), hex(t), hex(o))
        n = int.from_bytes(b[o + 1:o + 4], self.bo)
        if t == 0xC0:
            types = b[o + 4:o + 4 + n]
            vo = o + 4 + ((n + 3) & ~3)
            return [self.value(types[i], struct.unpack_from(self.e + "I", b, vo + 4 * i)[0], vo + 4 * i) for i in range(n)]
        if t == 0xC1:
            out = {}
            for i in range(n):
                e = o + 4 + 8 * i
                k = int.from_bytes(b[e:e + 3], self.bo)
                out[self.keys[k]] = self.value(b[e + 3], struct.unpack_from(self.e + "I", b, e + 4)[0], e)
            return out
        if t in (0x20, 0x21):  # hash -> value maps (u32 or u64 keys), types after the entries
            ks = 4 if t == 0x20 else 8
            es = ks + 4
            types = b[o + 4 + es * n:o + 4 + es * n + n]
            out = {}
            for i in range(n):
                e = o + 4 + es * i
                k = int.from_bytes(b[e:e + ks], self.bo)
                out[k] = self.value(types[i], struct.unpack_from(self.e + "I", b, e + ks)[0], e)
            return out
        raise ValueError(f"container type {t:#x}")


def load(b):
    return Byml(b).root
