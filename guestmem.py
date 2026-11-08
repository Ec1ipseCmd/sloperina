"""Locating the Switch's RAM inside the emulator process.

Lichii backs the console's physical RAM with one large mapped block (14 GiB
for this build). Everything the game stores lives somewhere in it, so values
are addressed here as offsets from the start of that block ("RAM offsets").
Whether a value's offset stays the same across scene loads and restarts is
what the scanner is for finding out."""

import ctypes
import ctypes.wintypes as wt

import win32

k = win32.kernel32
MIN_RAM_SIZE = 4 << 30
PAGE = 4096


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]


k.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
MEM_COMMIT, MEM_MAPPED = 0x1000, 0x40000


def find_ram(proc):
    """(host_address, size) of the guest RAM block, or None."""
    m, addr, best = MBI(), 0, None
    while k.VirtualQueryEx(proc.handle, addr, ctypes.byref(m), ctypes.sizeof(m)):
        if (m.Type == MEM_MAPPED and m.State == MEM_COMMIT and m.RegionSize >= MIN_RAM_SIZE
                and (best is None or m.RegionSize > best[1])):
            best = (m.BaseAddress, m.RegionSize)
        addr = (m.BaseAddress or 0) + m.RegionSize
        if addr >= 1 << 47:
            break
    return best


class _WSEntry(ctypes.Structure):
    _fields_ = [("va", ctypes.c_void_p), ("attr", ctypes.c_size_t)]


def resident_runs(proc, base, size):
    """[(offset, length)] of pages the emulator has actually touched.

    Reading untouched pages would make Windows allocate them, so scans only
    look at resident ones."""
    n = size // PAGE
    entries = (_WSEntry * n)()
    for i in range(n):
        entries[i].va = base + i * PAGE
    if not k.K32QueryWorkingSetEx(proc.handle, entries, ctypes.sizeof(entries)):
        raise OSError(ctypes.get_last_error(), "QueryWorkingSetEx failed")
    runs, start = [], None
    for i in range(n + 1):
        valid = i < n and entries[i].attr & 1
        if valid and start is None:
            start = i
        elif not valid and start is not None:
            runs.append((start * PAGE, (i - start) * PAGE))
            start = None
    return runs


class GuestRam:
    """Read/write values by RAM offset."""

    def __init__(self, proc):
        self.proc = proc
        found = find_ram(proc)
        if not found:
            raise LookupError("guest RAM block not found")
        self.base, self.size = found

    def read(self, offset, n):
        return self.proc.read(self.base + offset, n)

    def write(self, offset, data):
        if not 0 <= offset <= self.size - len(data):
            raise ValueError("offset outside guest RAM")
        self.proc.write(self.base + offset, data)

    def read_int(self, offset, size=4, signed=False):
        return int.from_bytes(self.read(offset, size), "little", signed=signed)

    def write_int(self, offset, value, size=4, signed=False):
        self.write(offset, int(value).to_bytes(size, "little", signed=signed))
