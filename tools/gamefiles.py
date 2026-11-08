"""Read romfs files from the dump: zstd with the game's dictionaries, SARC archives, BYML.

    python gamefiles.py ls <romfs path>      list a SARC archive (e.g. romfs/Pack/Actor/X.pack.zs)
    python gamefiles.py byml <romfs path> [member]   print a BYML file (or a member of a SARC) as JSON

Set OOTMENU_NUP to the dump's path (see nupfs.py). Needs zstandard.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import nupfs  # noqa: E402

_nup = None
_dicts = None


def nup():
    global _nup
    if _nup is None:
        _nup = nupfs.Nup(nupfs.NUP)
        _nup.index = {p: r for p, r in _nup.walk()}
    return _nup


def sarc(data):
    """{name: bytes} from a SARC archive."""
    assert data[:4] == b"SARC", "not SARC"
    e = "<" if data[6:8] == b"\xff\xfe" else ">"
    data_at = struct.unpack_from(e + "I", data, 0x0C)[0]
    sfat = struct.unpack_from(e + "H", data, 0x04)[0]
    assert data[sfat:sfat + 4] == b"SFAT"
    n = struct.unpack_from(e + "H", data, sfat + 6)[0]
    names_at = sfat + 0x0C + n * 0x10 + 8  # after the SFNT header
    out = {}
    for i in range(n):
        _, attr, start, end = struct.unpack_from(e + "IIII", data, sfat + 0x0C + i * 0x10)
        name = ""
        if attr & 0x01000000:
            o = names_at + (attr & 0xFFFF) * 4
            name = data[o:data.index(b"\0", o)].decode("utf-8")
        out[name] = data[data_at + start:data_at + end]
    return out


def dicts():
    global _dicts
    if _dicts is None:
        import zstandard
        raw = nup().read(nup().index["romfs/Pack/ZsDic.pack.zs"])
        if raw[:4] == nupfs.ZSTD:
            raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=1 << 26)
        _dicts = {k: zstandard.ZstdCompressionDict(v) for k, v in sarc(raw).items()}
    return _dicts


def unzs(path, data):
    """Decompress a .zs file with the dictionary its extension uses."""
    import zstandard
    name = path.rsplit("/", 1)[-1]
    d = dicts()
    if name.endswith(".bcett.byml.zs"):
        dic = d.get("bcett.byml.zsdic")
    elif name.endswith(".pack.zs"):
        dic = d.get("pack.zsdic")
    else:
        dic = d.get("zs.zsdic")
    z = zstandard.ZstdDecompressor(dict_data=dic) if dic else zstandard.ZstdDecompressor()
    return z.decompress(data, max_output_size=1 << 28)


def read(path):
    """A romfs file's contents, decompressed."""
    data = nup().read(nup().index[path])
    return unzs(path, data) if path.endswith(".zs") else data


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "ls":
        for k, v in sarc(read(a[1])).items():
            print(f"{len(v):>10}  {k}")
    elif len(a) >= 2 and a[0] == "byml":
        data = read(a[1])
        if len(a) > 2:
            data = sarc(data)[a[2]]
        print(json.dumps(byml.load(data), indent=1, default=str))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
