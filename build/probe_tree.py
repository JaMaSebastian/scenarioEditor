"""Quick probe: print the child tree of the ScenarioEditor main dialog."""
import ctypes
import ctypes.wintypes as wt
import time
from _e2e_common import (
    user32, launch_app, wait_for_window,
    enum_descendants,
)

p = launch_app()
hwnd = wait_for_window()
print(f"main hwnd=0x{hwnd:x}")
time.sleep(0.3)

count = 0
for h in enum_descendants(hwnd):
    cls = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(h, cls, 64)
    cid = user32.GetDlgCtrlID(h)
    parent = user32.GetParent(h)
    visible = user32.IsWindowVisible(h)
    print(f"  hwnd=0x{h:x} cls={cls.value!r} id={cid} parent=0x{parent:x} vis={visible}")
    count += 1

print(f"total={count}")

import subprocess
from _e2e_common import WM_CLOSE
user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
try: p.wait(timeout=5)
except: p.terminate()
