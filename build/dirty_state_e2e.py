"""Block J — dirty-state asterisk + Save/Discard prompt.

We use a kb-shortcut action (Ctrl+Alt+A → ID_KB_ADD_ASSET) to dirty the
scenario, because (a) it's wired end-to-end through the page's
NotifyDirty → WM_APP_MARK_DIRTY → dialog::MarkDirty path and (b) it does
not depend on cross-process WM_SETTEXT correctly firing EN_CHANGE.

  1. Clean settings.ini.
  2. Launch; title should NOT contain "*".
  3. Fire ID_KB_ADD_ASSET; title should gain " *".
  4. Send WM_CLOSE; expect MessageBox prompt "Save? Y/N/Cancel".
  5. Click No (Discard); app exits cleanly.
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time
from _e2e_common import (
    user32, WM_COMMAND, WM_CLOSE, IDNO,
    IDC_EDIT_SCENARIO_NAME, SETTINGS,
    launch_app, wait_for_window, find_ctrl, set_edit_text, get_title,
)


EN_CHANGE = 0x0300
ID_KB_ADD_ASSET = 32080


def find_messagebox(parent_pid, exclude_hwnd, timeout_s=3.0):
    """Find the modal MessageBox (class #32770) owned by our process,
    skipping the main dialog hwnd."""
    t_end = time.time() + timeout_s
    while time.time() < t_end:
        found = [0]
        EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, ctypes.c_void_p)
        def _cb(h, _):
            if h == exclude_hwnd: return True
            cls = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(h, cls, 64)
            if cls.value == "#32770":
                pid = ctypes.c_ulong()
                user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
                if pid.value == parent_pid:
                    found[0] = h; return False
            return True
        user32.EnumWindows(EnumProc(_cb), 0)
        if found[0]: return found[0]
        time.sleep(0.1)
    return 0


try: os.remove(SETTINGS)
except FileNotFoundError: pass

p = launch_app()
hwnd = wait_for_window()
if not hwnd:
    print("FAIL: window not found"); p.terminate(); sys.exit(1)
print(f"main hwnd=0x{hwnd:x}")
time.sleep(0.3)

fails = 0

t0 = get_title(hwnd)
print(f"initial title = {t0!r}")
if "*" in t0:
    print("FAIL: title shows * before any edit"); fails += 1

# Trigger a dirty-mutating action via the kb-shortcut command.
user32.SendMessageW(hwnd, WM_COMMAND, ID_KB_ADD_ASSET, 0)
time.sleep(0.3)

t1 = get_title(hwnd)
print(f"after edit title = {t1!r}")
if not t1.endswith(" *"):
    print("FAIL: title should end with ' *' after edit"); fails += 1

# Close → expect Save? prompt.
user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
mb = find_messagebox(p.pid, hwnd)
if not mb:
    print("FAIL: no Save? prompt appeared after WM_CLOSE")
    p.terminate(); fails += 1
else:
    print(f"prompt hwnd=0x{mb:x} title={get_title(mb)!r}")
    user32.PostMessageW(mb, WM_COMMAND, IDNO, 0)

try: p.wait(timeout=5)
except:
    print("FAIL: process did not exit"); p.terminate(); fails += 1

print("PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 5)
