"""Shared ctypes/Win32 helpers used by the E2E tests."""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import time

EXE_DIR  = r"D:\_work2\c++\unreal_5_7_4\ScenarioEditor\build\x64\Debug"
EXE      = os.path.join(EXE_DIR, "ScenarioEditor.exe")
SETTINGS = os.path.join(EXE_DIR, "settings.ini")

WM_COMMAND  = 0x0111
WM_SETTEXT  = 0x000C
WM_GETTEXT  = 0x000D
WM_CLOSE    = 0x0010
GW_CHILD    = 5
GW_HWNDNEXT = 2
IDCANCEL    = 2
IDNO        = 7

# Control IDs
IDC_EDIT_SCENARIO_NAME       = 1100
IDC_EDIT_ORIGIN_LAT          = 1111
IDC_EDIT_ORIGIN_LON          = 1112
IDC_EDIT_ORIGIN_ALT          = 1113
IDC_EDIT_ENTITY_LAT          = 1222
IDC_EDIT_ENTITY_LON          = 1223
IDC_EDIT_ENTITY_ALT          = 1224
IDC_EDIT_ENTITY_HEADING      = 1231
IDC_EDIT_ENTITY_PITCH        = 1232
IDC_EDIT_ENTITY_ROLL         = 1233

# Menu / command IDs
ID_FILE_NEW_SCENARIO         = 32000
ID_FILE_SAVE_SCENARIO        = 32002
ID_PLAYBACK_START            = 32020
ID_PLAYBACK_STOP             = 32023

# Status bar pane indices
PANE_STATE     = 0
PANE_TIME      = 1
PANE_PROGRESS  = 2
PANE_ERRORS    = 3
PANE_WARNINGS  = 4
PANE_INFO      = 5
PANE_PDUS      = 6
PANE_BW        = 7

# Win32 status-bar messages
SB_GETTEXTW       = 0x040D
SB_GETTEXTLENGTHW = 0x040C

PROCESS_VM_OP     = 0x0008
PROCESS_VM_READ   = 0x0010
PROCESS_VM_WRITE  = 0x0020
MEM_COMMIT        = 0x1000
MEM_RESERVE       = 0x2000
MEM_RELEASE       = 0x8000
PAGE_RW           = 0x04

user32   = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
user32.SendMessageW.restype  = ctypes.c_ssize_t
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_ssize_t, ctypes.c_ssize_t]
user32.PostMessageW.restype  = wt.BOOL
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, ctypes.c_ssize_t, ctypes.c_ssize_t]
user32.GetDlgCtrlID.restype  = ctypes.c_int
user32.GetWindow.restype     = wt.HWND
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.restype  = ctypes.c_int
user32.GetWindowRect.argtypes  = [wt.HWND, ctypes.POINTER(wt.RECT)]
user32.GetWindowRect.restype   = wt.BOOL
user32.SetWindowPos.argtypes   = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, wt.UINT]
user32.SetWindowPos.restype    = wt.BOOL
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(ctypes.c_ulong)]
user32.GetWindowThreadProcessId.restype  = ctypes.c_ulong
kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype  = wt.HANDLE
kernel32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t,
                                     wt.DWORD, wt.DWORD]
kernel32.VirtualAllocEx.restype  = ctypes.c_void_p
kernel32.VirtualFreeEx.argtypes  = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD]
kernel32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.ReadProcessMemory.restype  = wt.BOOL


def find_top(prefix="ScenarioEditor"):
    """Find the first top-level window whose title starts with prefix."""
    found = [0]
    EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, ctypes.c_void_p)
    def _cb(h, _):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value.startswith(prefix):
            found[0] = h; return False
        return True
    user32.EnumWindows(EnumProc(_cb), 0)
    return found[0]


def wait_for_window(prefix="ScenarioEditor", timeout_s=10.0):
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        h = find_top(prefix)
        if h: return h
        time.sleep(0.1)
    return 0


def enum_descendants(parent):
    seen = set()
    queue = [parent]
    while queue:
        h = queue.pop(0)
        if not h or h in seen: continue
        seen.add(h)
        yield h
        child = user32.GetWindow(h, GW_CHILD)
        while child:
            queue.append(child)
            child = user32.GetWindow(child, GW_HWNDNEXT)


def find_ctrl(parent, ctrl_id):
    for h in enum_descendants(parent):
        if user32.GetDlgCtrlID(h) == ctrl_id:
            return h
    return 0


def find_class(parent, cls):
    buf = ctypes.create_unicode_buffer(64)
    for h in enum_descendants(parent):
        user32.GetClassNameW(h, buf, 64)
        if buf.value == cls:
            return h
    return 0


def get_title(hwnd):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, buf, 256)
    return buf.value


def set_edit_text(hwnd, text):
    """SendMessage WM_SETTEXT with a wide string."""
    buf = ctypes.create_unicode_buffer(text)
    user32.SendMessageW(hwnd, WM_SETTEXT, 0, ctypes.addressof(buf))


def read_status_pane(status_hwnd, pane_idx):
    """Read a CStatusBar pane's text by cross-process VirtualAlloc + ReadProcessMemory."""
    pid = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(status_hwnd, ctypes.byref(pid))
    hproc = kernel32.OpenProcess(PROCESS_VM_OP | PROCESS_VM_READ | PROCESS_VM_WRITE,
                                  False, pid.value)
    if not hproc:
        return ""
    try:
        lenres = user32.SendMessageW(status_hwnd, SB_GETTEXTLENGTHW, pane_idx, 0)
        tlen = lenres & 0xFFFF
        if tlen <= 0:
            return ""
        nbytes = (tlen + 1) * 2
        remote = kernel32.VirtualAllocEx(hproc, None, nbytes,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_RW)
        if not remote:
            return ""
        try:
            user32.SendMessageW(status_hwnd, SB_GETTEXTW, pane_idx, remote)
            local = (ctypes.c_wchar * (tlen + 1))()
            read = ctypes.c_size_t(0)
            kernel32.ReadProcessMemory(hproc, remote, local, nbytes, ctypes.byref(read))
            return local.value
        finally:
            kernel32.VirtualFreeEx(hproc, remote, 0, MEM_RELEASE)
    finally:
        kernel32.CloseHandle(hproc)


def launch_app():
    return subprocess.Popen([EXE], cwd=EXE_DIR)


def shutdown_app(p, hwnd):
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try: p.wait(timeout=5)
    except subprocess.TimeoutExpired: p.terminate()
