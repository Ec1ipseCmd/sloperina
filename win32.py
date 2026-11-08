"""Thin ctypes wrappers for the Win32 bits ootmenu needs:
finding the emulator process/window, overlay window styles, keyboard and
XInput polling, sending a key to the emulator, and process memory access."""

import ctypes
import ctypes.wintypes as wt

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

# ---------------------------------------------------------------- DPI

def make_dpi_aware():
    try:
        ctypes.WinDLL("shcore").SetProcessDpiAwareness(2)  # per-monitor
    except Exception:
        try:
            user32.SetProcessDPIAware()
        except Exception:
            pass

# ---------------------------------------------------------------- processes

TH32CS_SNAPPROCESS = 0x2


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
        ("szExeFile", wt.WCHAR * 260),
    ]


kernel32.CreateToolhelp32Snapshot.restype = wt.HANDLE
kernel32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.CloseHandle.argtypes = [wt.HANDLE]


def find_pid(exe_name):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == wt.HANDLE(-1).value:
        return None
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            if entry.szExeFile.lower() == exe_name.lower():
                return entry.th32ProcessID
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return None

PROCESS_TERMINATE = 0x1
ERROR_ALREADY_EXISTS = 183
kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.LPCWSTR]
kernel32.CreateMutexW.restype = wt.HANDLE
kernel32.TerminateProcess.argtypes = [wt.HANDLE, wt.UINT]
user32.MessageBoxW.argtypes = [wt.HWND, wt.LPCWSTR, wt.LPCWSTR, wt.UINT]


def claim_single_instance(name):
    """True if we are the only holder of the named mutex (kept for process lifetime)."""
    global _instance_mutex
    _instance_mutex = kernel32.CreateMutexW(None, False, name)
    return ctypes.get_last_error() != ERROR_ALREADY_EXISTS


def kill_process(pid):
    h = kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if h:
        kernel32.TerminateProcess(h, 0)
        kernel32.CloseHandle(h)


def ask_yes_no(title, text):
    MB_YESNO, MB_ICONQUESTION, MB_TOPMOST, IDYES = 0x4, 0x20, 0x40000, 6
    return user32.MessageBoxW(None, text, title, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES


def screen_work_area():
    """(x, y, w, h) of the primary monitor's work area."""
    r = wt.RECT()
    user32.SystemParametersInfoW(0x30, 0, ctypes.byref(r), 0)  # SPI_GETWORKAREA
    return r.left, r.top, r.right - r.left, r.bottom - r.top

# ---------------------------------------------------------------- windows

WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wt.LPARAM]
user32.EnumChildWindows.argtypes = [wt.HWND, WNDENUMPROC, wt.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.ClientToScreen.argtypes = [wt.HWND, ctypes.POINTER(wt.POINT)]
user32.GetForegroundWindow.restype = wt.HWND
user32.GetAncestor.argtypes = [wt.HWND, wt.UINT]
user32.GetAncestor.restype = wt.HWND
user32.GetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int]
user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.SetWindowLongPtrW.argtypes = [wt.HWND, ctypes.c_int, ctypes.c_ssize_t]
user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.IsIconic.argtypes = [wt.HWND]


def main_window_of(pid):
    """Largest visible top-level window owned by pid."""
    best = [None, 0]

    def cb(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            r = wt.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(r))
            area = (r.right - r.left) * (r.bottom - r.top)
            if area > best[1]:
                best[0], best[1] = hwnd, area
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return best[0]


def client_rect_on_screen(hwnd):
    """(x, y, w, h) of hwnd's client area in screen pixels."""
    r = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(r))
    p = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y, r.right - r.left, r.bottom - r.top


def render_rect(hwnd):
    """(x, y, w, h) on screen of the emulator's picture: its largest visible child window (Lichii draws
    the game in a child below the menu bar), else the client area."""
    best = [None, 0]

    def cb(child, _):
        if user32.IsWindowVisible(child):
            r = wt.RECT()
            user32.GetWindowRect(child, ctypes.byref(r))
            area = (r.right - r.left) * (r.bottom - r.top)
            if area > best[1]:
                best[0], best[1] = (r.left, r.top, r.right - r.left, r.bottom - r.top), area
        return True

    user32.EnumChildWindows(hwnd, WNDENUMPROC(cb), 0)
    return best[0] or client_rect_on_screen(hwnd)


kernel32.GetCurrentProcess.restype = wt.HANDLE
kernel32.GetCurrentThread.restype = wt.HANDLE
kernel32.SetPriorityClass.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.GetPriorityClass.argtypes = [wt.HANDLE]
kernel32.SetThreadPriority.argtypes = [wt.HANDLE, ctypes.c_int]
kernel32.GetThreadPriority.argtypes = [wt.HANDLE]
ABOVE_NORMAL_PRIORITY_CLASS = 0x8000
THREAD_PRIORITY_HIGHEST = 2


def raise_priority():
    """Run this process above normal priority, so a busy emulator in the foreground doesn't starve it."""
    kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)


def raise_thread_priority():
    """Highest priority for the calling thread (the levitate loop's millisecond writes)."""
    kernel32.SetThreadPriority(kernel32.GetCurrentThread(), THREAD_PRIORITY_HIGHEST)


def foreground_window():
    return user32.GetForegroundWindow()


def is_minimized(hwnd):
    return bool(user32.IsIconic(hwnd))


GA_ROOT = 2
GWL_EXSTYLE = -20
WS_EX_TOPMOST = 0x8
WS_EX_TRANSPARENT = 0x20
WS_EX_TOOLWINDOW = 0x80
WS_EX_LAYERED = 0x80000
WS_EX_NOACTIVATE = 0x8000000
SW_HIDE = 0
SW_SHOWNOACTIVATE = 4


def root_hwnd(tk_widget_id):
    return user32.GetAncestor(tk_widget_id, GA_ROOT)


def make_overlay(hwnd):
    """Click-through, never takes focus, hidden from the taskbar."""
    style = user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
    style |= (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW
              | WS_EX_NOACTIVATE | WS_EX_TOPMOST)
    user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)


user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, wt.UINT]
user32.GetWindowRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
HWND_TOPMOST = wt.HWND(-1)
SWP_NOACTIVATE = 0x10


def move_topmost(hwnd, x, y, w, h):
    """Place hwnd at screen pixels (x, y), size w x h, above normal windows, without focusing it."""
    user32.SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)


def window_rect(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top


def show_no_activate(hwnd, show):
    user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE if show else SW_HIDE)

# ---------------------------------------------------------------- keyboard

user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
user32.GetAsyncKeyState.restype = ctypes.c_short

VK = {
    "BACKSPACE": 0x08, "ENTER": 0x0D, "SHIFT": 0x10, "CTRL": 0x11, "ESC": 0x1B,
    "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27, "DOWN": 0x28, "INSERT": 0x2D,
    **{f"F{i}": 0x6F + i for i in range(1, 13)},
}


def key_down(name):
    vk = VK.get(name.upper())
    return vk is not None and bool(user32.GetAsyncKeyState(vk) & 0x8000)


KEYEVENTF_KEYUP = 0x2
user32.MapVirtualKeyW.argtypes = [wt.UINT, wt.UINT]


def tap_key(name):
    """Send a key press+release to whatever window has focus."""
    vk = VK[name.upper()]
    scan = user32.MapVirtualKeyW(vk, 0)
    user32.keybd_event(vk, scan, 0, 0)
    user32.keybd_event(vk, scan, KEYEVENTF_KEYUP, 0)

# ---------------------------------------------------------------- XInput

class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [("wButtons", wt.WORD), ("bLeftTrigger", ctypes.c_ubyte),
                ("bRightTrigger", ctypes.c_ubyte), ("sThumbLX", ctypes.c_short),
                ("sThumbLY", ctypes.c_short), ("sThumbRX", ctypes.c_short),
                ("sThumbRY", ctypes.c_short)]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [("dwPacketNumber", wt.DWORD), ("Gamepad", XINPUT_GAMEPAD)]


_xinput = None
for _dll in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
    try:
        _xinput = ctypes.WinDLL(_dll)
        break
    except OSError:
        continue

XBUTTONS = {
    "DUP": 0x0001, "DDOWN": 0x0002, "DLEFT": 0x0004, "DRIGHT": 0x0008,
    "START": 0x0010, "BACK": 0x0020, "LSTICK": 0x0040, "RSTICK": 0x0080,
    "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000,
}
STICK_THRESHOLD = 16000   # of 32767
last_sticks = (0, 0, 0, 0, 0, 0)  # raw LX, LY, RX, RY, LT, RT from the last read_pad()
TRIGGER_THRESHOLD = 128   # of 255


def read_pad():
    """Set of pressed button names on the first connected pad (may be empty).

    Besides the face/shoulder buttons, analog inputs are exposed as virtual
    buttons: LT, RT, and C-UP/C-DOWN/C-LEFT/C-RIGHT for the right stick."""
    global last_sticks
    last_sticks = (0, 0, 0, 0, 0, 0)
    if _xinput is None:
        return set()
    state = XINPUT_STATE()
    for port in range(4):
        if _xinput.XInputGetState(port, ctypes.byref(state)) == 0:
            break
    else:
        return set()
    g = state.Gamepad
    last_sticks = (g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY, g.bLeftTrigger, g.bRightTrigger)
    pressed = {name for name, bit in XBUTTONS.items() if g.wButtons & bit}
    if g.bLeftTrigger > TRIGGER_THRESHOLD:
        pressed.add("LT")
    if g.bRightTrigger > TRIGGER_THRESHOLD:
        pressed.add("RT")
    if g.sThumbRY > STICK_THRESHOLD:
        pressed.add("C-UP")
    if g.sThumbRY < -STICK_THRESHOLD:
        pressed.add("C-DOWN")
    if g.sThumbRX < -STICK_THRESHOLD:
        pressed.add("C-LEFT")
    if g.sThumbRX > STICK_THRESHOLD:
        pressed.add("C-RIGHT")
    return pressed

# ---------------------------------------------------------------- memory

PROCESS_VM_READ = 0x10
PROCESS_VM_WRITE = 0x20
PROCESS_VM_OPERATION = 0x8
PROCESS_QUERY_INFORMATION = 0x400

kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype = wt.HANDLE
kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + [
        (n, ctypes.c_size_t) for n in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
            "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]


class Process:
    """Handle to the emulator process for reading/writing host memory.

    Guest (Switch) addresses are NOT host addresses: a future step is to locate
    where the emulator maps guest memory, then translate guest -> host."""

    def __init__(self, pid):
        self.pid = pid
        self.handle = kernel32.OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION
            | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess failed")

    def read(self, address, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not kernel32.ReadProcessMemory(self.handle, address, buf, size, ctypes.byref(n)):
            raise OSError(ctypes.get_last_error(), f"read failed at {address:#x}")
        return buf.raw[:n.value]

    def write(self, address, data):
        n = ctypes.c_size_t()
        if not kernel32.WriteProcessMemory(self.handle, address, data, len(data), ctypes.byref(n)):
            raise OSError(ctypes.get_last_error(), f"write failed at {address:#x}")

    def working_set_bytes(self):
        c = PROCESS_MEMORY_COUNTERS()
        c.cb = ctypes.sizeof(c)
        if kernel32.K32GetProcessMemoryInfo(self.handle, ctypes.byref(c), c.cb):
            return c.WorkingSetSize
        return 0

    def close(self):
        if self.handle:
            kernel32.CloseHandle(self.handle)
            self.handle = None
