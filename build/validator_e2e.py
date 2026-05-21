"""Block H — validation engine wired into the status bar.

  1. Write a scenario.ini with two entities sharing the same (Site, App,
     Entity) tuple — the Validator must flag a duplicate-ID error.
  2. Seed settings.ini so the dialog auto-loads that fixture.
  3. Launch ScenarioEditor; find the status bar.
  4. Read panes 3 (Errors) and 6 (PDUs) via SB_GETTEXTW.
  5. Assert Errors >= 1 and PDUs == 600 (2 enabled entities × 5 Hz × 60 s).
"""
import os
import sys
import time
from _e2e_common import (
    user32, WM_CLOSE, EXE, EXE_DIR, SETTINGS,
    launch_app, wait_for_window, find_class, read_status_pane,
    PANE_ERRORS, PANE_PDUS, PANE_WARNINGS,
)


FIXTURE = os.path.join(EXE_DIR, "validator_fixture.ini")

# 1. Broken scenario: two entities with same Site/App/Entity tuple.
fixture_text = """[Format]
Version=1

[Scenario]
Name=ValidatorFixture
DurationSeconds=60
DefaultUpdateRateHz=5

[Entities]
Count=2

[Entity.1]
Name=A
SiteId=1
ApplicationId=1
EntityId=42
Enabled=1
Lat=10.0
Lon=20.0
Alt=100
HeadingDeg=0
PitchDeg=0
RollDeg=0
InitialCoordMode=LatLonAlt

[Entity.2]
Name=B
SiteId=1
ApplicationId=1
EntityId=42
Enabled=1
Lat=10.0
Lon=20.0
Alt=100
HeadingDeg=0
PitchDeg=0
RollDeg=0
InitialCoordMode=LatLonAlt
"""
with open(FIXTURE, "w", encoding="ascii") as f:
    f.write(fixture_text)

# 2. Seed settings.ini to auto-load the fixture.
settings_text = (
    "[Format]\nVersion=1\n"
    "[Window]\nPositionX=200\nPositionY=200\nWidth=1400\nHeight=900\nMaximized=0\n"
    "[Paths]\n"
    f"LastScenario={FIXTURE}\n"
)
with open(SETTINGS, "w", encoding="ascii") as f:
    f.write(settings_text)

# 3. Launch.
p = launch_app()
hwnd = wait_for_window()
if not hwnd:
    print("FAIL: window not found"); p.terminate(); sys.exit(1)
print(f"main hwnd=0x{hwnd:x}")

# Give the auto-load + Revalidate() time to land.
time.sleep(0.6)

status = find_class(hwnd, "msctls_statusbar32")
if not status:
    print("FAIL: status bar not found"); user32.PostMessageW(hwnd, WM_CLOSE, 0, 0); p.wait(); sys.exit(2)

errors_text   = read_status_pane(status, PANE_ERRORS)
warnings_text = read_status_pane(status, PANE_WARNINGS)
pdus_text     = read_status_pane(status, PANE_PDUS)
print(f"errors pane   = {errors_text!r}")
print(f"warnings pane = {warnings_text!r}")
print(f"pdus pane     = {pdus_text!r}")

fails = 0
if "Errors:" not in errors_text:
    print("FAIL: errors pane format unexpected"); fails += 1
else:
    try:
        n = int(errors_text.split(":")[1].strip())
        if n < 1:
            print(f"FAIL: expected >=1 errors, got {n}"); fails += 1
    except ValueError:
        print(f"FAIL: could not parse errors count from {errors_text!r}"); fails += 1

if "PDUs:" not in pdus_text:
    print("FAIL: PDUs pane format unexpected"); fails += 1
else:
    try:
        n = int(pdus_text.split(":")[1].strip())
        if n != 600:
            print(f"FAIL: expected 600 PDUs, got {n}"); fails += 1
    except ValueError:
        print(f"FAIL: could not parse PDU count from {pdus_text!r}"); fails += 1

user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
try: p.wait(timeout=5)
except: p.terminate()

try: os.remove(FIXTURE)
except OSError: pass

print("PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 5)
