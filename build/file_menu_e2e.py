"""File menu smoke test.

  1. Launch app; verify title starts with "ScenarioEditor".
  2. Type "Hello Save" into the Scenario Name edit.
  3. Fire ID_FILE_SAVE_SCENARIO. No current path → falls through to Save As
     and pops a CFileDialog. Detect new top-level "Save..." window.
  4. Cancel Save As (post WM_COMMAND IDCANCEL).
  5. Fire ID_FILE_NEW_SCENARIO; verify title still starts with
     "ScenarioEditor".
  6. Regression: fire ID_PLAYBACK_START; expect a PDU.
"""
import ctypes
import ctypes.wintypes as wt
import socket
import sys
import time
from _e2e_common import (
    user32, WM_COMMAND, IDCANCEL,
    IDC_EDIT_SCENARIO_NAME, ID_FILE_NEW_SCENARIO, ID_FILE_SAVE_SCENARIO,
    ID_PLAYBACK_START, launch_app, wait_for_window, shutdown_app,
    find_ctrl, set_edit_text, get_title,
)


p = launch_app()
top = wait_for_window()
if not top:
    print("FAIL: ScenarioEditor window not found", file=sys.stderr)
    p.terminate(); sys.exit(1)
time.sleep(0.8)   # let OnInitDialog complete creating all child controls
print(f"Main window: 0x{top:x} title={get_title(top)!r}")

fails = 0

# 2. Type into Scenario Name.
name_h = find_ctrl(top, IDC_EDIT_SCENARIO_NAME)
if name_h:
    set_edit_text(name_h, "Hello Save")
    print("set Scenario Name = 'Hello Save'")
else:
    print("FAIL: Scenario Name edit not found"); fails += 1

# 3. File > Save → Save As dialog.
user32.PostMessageW(top, WM_COMMAND, ID_FILE_SAVE_SCENARIO, 0)
print("posted ID_FILE_SAVE_SCENARIO")
time.sleep(0.6)

# Enumerate top-level windows whose title contains "Save".
save_dlg = 0
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, ctypes.c_void_p)
def _walk(h, _):
    global save_dlg
    t = get_title(h)
    if t and ("Save" in t) and t != get_title(top):
        if not save_dlg:
            save_dlg = h
    return True
user32.EnumWindows(EnumWindowsProc(_walk), 0)

if save_dlg:
    print(f"Save As dialog detected: 0x{save_dlg:x} title={get_title(save_dlg)!r}")
    user32.PostMessageW(save_dlg, WM_COMMAND, IDCANCEL, 0)
    time.sleep(0.3)
    print("cancelled Save As dialog")
else:
    print("FAIL: no Save As dialog appeared after ID_FILE_SAVE_SCENARIO")
    fails += 1

# 5. File > New → title resets.
user32.SendMessageW(top, WM_COMMAND, ID_FILE_NEW_SCENARIO, 0)
time.sleep(0.2)
new_title = get_title(top)
print(f"after File > New, title = {new_title!r}")
if not new_title.startswith("ScenarioEditor"):
    print("FAIL: title did not reset"); fails += 1

# 6. Regression: Playback Start still fires.
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 3000)); sock.settimeout(5.0)
user32.SendMessageW(top, WM_COMMAND, ID_PLAYBACK_START, 0)
try:
    data, _ = sock.recvfrom(2048)
    print(f"Playback Start: received {len(data)} bytes (regression OK)")
    if data[0] != 0x07 or data[2] != 0x01:
        print("FAIL: PDU header wrong"); fails += 1
except socket.timeout:
    print("FAIL: no PDU after Playback Start"); fails += 1
finally:
    sock.close()

shutdown_app(p, top)

print("PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 2)
