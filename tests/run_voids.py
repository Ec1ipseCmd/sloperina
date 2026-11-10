#!/usr/bin/env python3
"""Run void sensor/control tests and verify generated IPS dispatch/fallback instructions."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def check_patch(path):
    data = path.read_bytes()
    assert data[:5] == b"IPS32" and data[-4:] == b"EEOF"
    records = {}
    at = 5
    while at < len(data) - 4:
        address, size = struct.unpack_from(">IH", data, at)
        assert size and address >= 0x100
        address -= 0x100
        assert address not in records
        records[address] = data[at + 6:at + 6 + size]
        at += 6 + size
    assert at == len(data) - 4 and len(records) == 9 and len(data) == 183
    assert records[0x3C46400] == b"OOTGZ\0\0\0" + struct.pack("<II", 3, 0)
    occupied = set()
    for address, payload in records.items():
        span = set(range(address, address + len(payload)))
        assert not span & occupied
        occupied |= span

    def branch(pc, word, link=False):
        assert word >> 26 == (0x25 if link else 5)
        return pc + signed(word & 0x3FFFFFF, 26) * 4

    for site, stub, slot, original, entry in (
        (0x3B500DC, 0x3C46440, 0x4E9A4D0, 0x3C42470, False),
        (0x2654B20, 0x3C46460, 0x4E9A4D8, 0x2654B24, True),
        (0x31E6A54, 0x3C46480, 0x4E9A4E0, 0x373A20, False),
        (0x82C690, 0x3C464A0, 0x4E9A4E8, 0x82C694, True),
    ):
        assert branch(site, struct.unpack("<I", records[site])[0], site == 0x31E6A54) == stub
        words = struct.unpack("<" + "I" * (len(records[stub]) // 4), records[stub])
        adrp, ldr, cbz, br = words[:4]
        assert adrp & 0x9F00001F == 0x90000010
        page = (stub & ~0xFFF) + signed(((adrp >> 5 & 0x7FFFF) << 2) | (adrp >> 29 & 3), 21) * 4096
        assert ldr & 0xFFC003FF == 0xF9400210
        assert page + (ldr >> 10 & 0xFFF) * 8 == slot
        assert cbz & 0xFF00001F == 0xB4000010
        assert stub + 8 + signed(cbz >> 5 & 0x7FFFF, 19) * 4 == stub + 16
        assert br == 0xD61F0200  # BR x16: keep x0..x3, stack and LR intact
        if entry:
            assert words[4] == (0xB9191E68 if site == 0x82C690 else 0xD503237F)
            assert branch(stub + 20, words[5]) == original
        else:
            assert branch(stub + 16, words[4]) == original
    print("IPS: disjoint records, hook branches, slots, original fallbacks and PACIBSP trampoline passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    suffix = ".exe" if os.name == "nt" else ""
    for name, source in (("voids", "tests/voids.c"), ("void_geometry", "tests/void_geometry.c"),
                         ("mkips_test", "src/host/mkips.c")):
        exe = out / (name + suffix)
        extra = (["src/ootgz/world_view.c", "src/ootgz/void_view.c", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
                 if name == "void_geometry" else [])
        for command in ([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/ootgz",
                         source] + extra + ["-lm", "-o", str(exe)],
                        [str(exe)] + ([str(out / "voids.ips")] if name == "mkips_test" else [])):
            result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **flags)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            result.check_returncode()
    check_patch(out / "voids.ips")


if __name__ == "__main__":
    main()
