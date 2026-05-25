"""End-to-end test for Lat/Lon/Alt -> ECEF wire conversion.

  1. Spawn ScenarioEditor + UDP listener on 127.0.0.1:3000.
  2. Drive Entity Lat/Lon/Alt to London (51.5074, 0.1278, 25 m). Coord-mode
     default is already Lat/Lon/Alt so no combo flip needed.
  3. Fire Playback > Start, receive ONE PDU.
  4. Decode the ECEF location (offset 48, 3*float64 BE per DIS).
  5. Compare against expected ECEF and exit 0 on match.
"""
import ctypes
import socket
import struct
import sys
import time
from _e2e_common import (
    user32, WM_COMMAND, ID_PLAYBACK_START,
    IDC_EDIT_ENTITY_LAT, IDC_EDIT_ENTITY_LON, IDC_EDIT_ENTITY_ALT,
    IDC_EDIT_ENTITY_HEADING, IDC_EDIT_ENTITY_PITCH, IDC_EDIT_ENTITY_ROLL,
    IDC_EDIT_ORIGIN_LAT, IDC_EDIT_ORIGIN_LON, IDC_EDIT_ORIGIN_ALT,
    launch_app, wait_for_window, shutdown_app, find_ctrl, set_edit_text,
)


EXPECTED_ECEF = (3978009.83, 8873.09, 4968894.50)
EXPECTED_PSI_THETA_PHI = (1.5730, 0.0000, -2.4698)


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 3000)); sock.settimeout(5.0)

p = launch_app()
hwnd = wait_for_window()
if not hwnd:
    print("FAIL: window not found", file=sys.stderr); p.terminate(); sys.exit(1)
print(f"found ScenarioEditor hwnd=0x{hwnd:x}")
time.sleep(0.8)   # let pages finish OnInitDialog so their edits exist

fails = 0

# Flip the Entity's Initial Coord Mode combo to LatLonAlt (index 0) so the
# single dynamic position row is interpreted as Lat / Lon / Alt. After the
# Initial State refactor (Local/ECEF edits removed), the three visible
# edits IDC_EDIT_ENTITY_LAT/LON/ALT are polymorphic.
IDC_COMBO_ENTITY_COORD_MODE = 1221
CB_SETCURSEL = 0x014E
CBN_SELCHANGE = 1
WM_COMMAND_HI_SELCHANGE = (CBN_SELCHANGE << 16)
h_combo = find_ctrl(hwnd, IDC_COMBO_ENTITY_COORD_MODE)
if h_combo:
    user32.SendMessageW(h_combo, CB_SETCURSEL, 0, 0)   # 0 = LatLonAlt
    # Synthesize CBN_SELCHANGE so the page's handler relabels + repopulates.
    parent_hwnd = user32.GetParent(h_combo)
    user32.SendMessageW(parent_hwnd, WM_COMMAND,
                        (CBN_SELCHANGE << 16) | IDC_COMBO_ENTITY_COORD_MODE,
                        h_combo)
    time.sleep(0.2)
else:
    print("WARN: IDC_COMBO_ENTITY_COORD_MODE not found")

# Set origin = entity (so ENU offset is (0,0,0) and the heading lerp uses
# the entity's geodetic for psi/theta/phi).
for cid, val in [
    (IDC_EDIT_ORIGIN_LAT,  "51.5074"),
    (IDC_EDIT_ORIGIN_LON,  "0.1278"),
    (IDC_EDIT_ORIGIN_ALT,  "25"),
    (IDC_EDIT_ENTITY_LAT,  "51.5074"),
    (IDC_EDIT_ENTITY_LON,  "0.1278"),
    (IDC_EDIT_ENTITY_ALT,  "25"),
    (IDC_EDIT_ENTITY_HEADING, "90"),
    (IDC_EDIT_ENTITY_PITCH,   "0"),
    (IDC_EDIT_ENTITY_ROLL,    "0"),
]:
    h = find_ctrl(hwnd, cid)
    if not h:
        print(f"WARN: ctrl id={cid} not found")
        continue
    set_edit_text(h, val)
    # Read back to verify the text actually landed.
    rb = ctypes.create_unicode_buffer(64)
    user32.SendMessageW(h, 0x000D, 64, ctypes.addressof(rb))   # WM_GETTEXT
    if rb.value != val:
        print(f"WARN: ctrl id={cid} expected '{val}' got '{rb.value}'")

time.sleep(0.3)
user32.PostMessageW(hwnd, WM_COMMAND, ID_PLAYBACK_START, 0)
print("posted Playback > Start")

try:
    data, _ = sock.recvfrom(2048)
except socket.timeout:
    print("FAIL: no PDU received after Playback Start"); shutdown_app(p, hwnd); sys.exit(2)

print(f"received {len(data)} bytes")

# DIS EntityStatePdu: ECEF location at offset 48, 3 big-endian doubles.
ex, ey, ez = struct.unpack(">ddd", data[48:72])
print(f"PDU ECEF: ({ex:.2f}, {ey:.2f}, {ez:.2f})")
print(f"expected: ({EXPECTED_ECEF[0]:.2f}, {EXPECTED_ECEF[1]:.2f}, {EXPECTED_ECEF[2]:.2f})")
dx, dy, dz = abs(ex - EXPECTED_ECEF[0]), abs(ey - EXPECTED_ECEF[1]), abs(ez - EXPECTED_ECEF[2])
print(f"delta:    ({dx:.3f}, {dy:.3f}, {dz:.3f}) m  (tol 0.5 m)")
if max(dx, dy, dz) > 0.5:
    print("FAIL: ECEF outside tolerance"); fails += 1

# psi/theta/phi: 3 big-endian floats at offset 72.
psi, theta, phi = struct.unpack(">fff", data[72:84])
print(f"PDU psi/theta/phi: ({psi:.4f}, {theta:.4f}, {phi:.4f})")
print(f"expected:          ({EXPECTED_PSI_THETA_PHI[0]:.4f}, {EXPECTED_PSI_THETA_PHI[1]:.4f}, {EXPECTED_PSI_THETA_PHI[2]:.4f})")
dp = abs(psi - EXPECTED_PSI_THETA_PHI[0])
dt = abs(theta - EXPECTED_PSI_THETA_PHI[1])
df = abs(phi - EXPECTED_PSI_THETA_PHI[2])
print(f"delta:             ({dp:.5f}, {dt:.5f}, {df:.5f}) rad  (tol 0.001)")
if max(dp, dt, df) > 0.001:
    print("FAIL: psi/theta/phi outside tolerance"); fails += 1

shutdown_app(p, hwnd)
sock.close()

print("PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 5)
