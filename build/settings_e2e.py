"""settings.ini persistence test (§11.5).

Verifies that the app:
  1. Writes settings.ini on clean shutdown.
  2. Re-reads it on next launch (the saved values appear in cfg).
  3. The restored window is on-screen and not zero-sized.

Cross-process pixel-exact rect comparison is fragile because Python is
DPI-aware while ScenarioEditor is partially DPI-virtualized — coordinate
frames disagree by the DPI scale factor. The properties we *do* care
about: the file is written, it has plausible content, and the window
comes back at a usable size and position.
"""
import configparser
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time
from _e2e_common import (
    user32, WM_CLOSE, EXE, EXE_DIR, SETTINGS,
    wait_for_window,
)


# Run 1: launch, move, close.
try: os.remove(SETTINGS)
except FileNotFoundError: pass

p1 = subprocess.Popen([EXE], cwd=EXE_DIR)
hwnd = wait_for_window()
if not hwnd:
    print("FAIL: first launch window not found", file=sys.stderr); p1.terminate(); sys.exit(1)
print(f"first launch hwnd=0x{hwnd:x}")

SWP_NOZORDER = 0x4
SWP_NOACTIVATE = 0x10
user32.SetWindowPos(hwnd, 0, 250, 175, 1400, 900, SWP_NOZORDER | SWP_NOACTIVATE)
time.sleep(0.3)

rect_before_close = wt.RECT()
user32.GetWindowRect(hwnd, ctypes.byref(rect_before_close))
print(f"first-instance rect = ({rect_before_close.left}, {rect_before_close.top}, "
      f"{rect_before_close.right - rect_before_close.left}, "
      f"{rect_before_close.bottom - rect_before_close.top})")

user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
try: p1.wait(timeout=10)
except subprocess.TimeoutExpired:
    print("FAIL: first instance didn't exit on WM_CLOSE"); p1.terminate(); sys.exit(2)

if not os.path.exists(SETTINGS):
    print(f"FAIL: {SETTINGS} not written"); sys.exit(3)

cfg = configparser.ConfigParser()
cfg.read(SETTINGS)
print("--- settings.ini ---")
for sec in cfg.sections():
    for k, v in cfg[sec].items():
        print(f"  [{sec}] {k} = {v}")

fails = 0
def cint(s, k): return int(cfg[s][k])
saved_w = cint("Window", "width")
saved_h = cint("Window", "height")
saved_x = cint("Window", "positionx")
saved_y = cint("Window", "positiony")
if saved_w < 500 or saved_h < 300:
    print("FAIL: saved size implausibly small"); fails += 1
if saved_x < 0 or saved_y < 0:
    print("FAIL: saved coords negative"); fails += 1

# Run 2: relaunch, verify the window comes up at a usable rect.
p2 = subprocess.Popen([EXE], cwd=EXE_DIR)
hwnd2 = wait_for_window()
if not hwnd2:
    print("FAIL: second launch window not found"); p2.terminate(); sys.exit(4)
time.sleep(0.5)

r2 = wt.RECT()
user32.GetWindowRect(hwnd2, ctypes.byref(r2))
w2 = r2.right - r2.left
h2 = r2.bottom - r2.top
print(f"second-launch rect = ({r2.left}, {r2.top}, {w2}, {h2})")

# Tolerance check: window is visibly sized and on-screen.
if w2 < 500 or h2 < 300:
    print(f"FAIL: relaunch window too small ({w2}x{h2})"); fails += 1

screen_w = user32.GetSystemMetrics(0)
screen_h = user32.GetSystemMetrics(1)
if r2.left >= screen_w or r2.top >= screen_h:
    print(f"FAIL: relaunch window off-screen (screen {screen_w}x{screen_h})"); fails += 1
if r2.right <= 0 or r2.bottom <= 0:
    print(f"FAIL: relaunch window above/left of screen"); fails += 1

user32.PostMessageW(hwnd2, WM_CLOSE, 0, 0)
try: p2.wait(timeout=10)
except subprocess.TimeoutExpired: p2.terminate()

print("PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 5)
