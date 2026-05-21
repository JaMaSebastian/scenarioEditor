"""5 Hz worker-heartbeat sanity test (§18/§19).

  1. Spawn ScenarioEditor + UDP listener on 127.0.0.1:3000.
  2. Fire Playback > Start; receive PDUs for 1.5 s.
  3. Expect ~5 Hz (5 to 10 PDUs).
  4. Fire Playback > Stop; 0.5 s drain; expect zero further PDUs.
  5. Fire Playback > Start again — verifies the worker can restart.
"""
import socket
import sys
import time
from _e2e_common import (
    user32, WM_COMMAND, ID_PLAYBACK_START, ID_PLAYBACK_STOP,
    launch_app, wait_for_window, shutdown_app,
)


def count_pdus(sock, duration_s):
    count = 0
    end = time.time() + duration_s
    sock.settimeout(0.05)
    while time.time() < end:
        try:
            sock.recv(2048); count += 1
        except socket.timeout:
            pass
    return count


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 3000))

p = launch_app()
hwnd = wait_for_window()
if not hwnd:
    print("FAIL: window not found", file=sys.stderr); p.terminate(); sys.exit(1)
print(f"found hwnd=0x{hwnd:x}")

fails = 0

# 1) Start, count 1.5 s
user32.PostMessageW(hwnd, WM_COMMAND, ID_PLAYBACK_START, 0)
n1 = count_pdus(sock, 1.5)
print(f"interval 1: received {n1} PDUs over 1.5 s (expect 5..10)")
if not (5 <= n1 <= 10):
    print("FAIL: interval-1 PDU count out of range"); fails += 1

# 2) Stop, then expect quiet
user32.PostMessageW(hwnd, WM_COMMAND, ID_PLAYBACK_STOP, 0)
# Drain any in-flight PDUs already queued in kernel buffer.
drained = count_pdus(sock, 0.3)
print(f"drained {drained} residual after Stop")
n2 = count_pdus(sock, 0.5)
print(f"interval 2 (after Stop): {n2} PDUs (expect 0)")
if n2 != 0:
    print("FAIL: PDUs received after Stop"); fails += 1

# 3) Re-Start
user32.PostMessageW(hwnd, WM_COMMAND, ID_PLAYBACK_START, 0)
n3 = count_pdus(sock, 1.0)
print(f"interval 3 (restarted): {n3} PDUs over 1.0 s (expect 3..7)")
if not (3 <= n3 <= 7):
    print("FAIL: interval-3 PDU count out of range"); fails += 1

shutdown_app(p, hwnd)
sock.close()

print(f"\n{'PASS' if fails == 0 else f'FAIL ({fails})'}")
sys.exit(0 if fails == 0 else 5)
