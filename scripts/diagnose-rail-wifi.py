"""Capture ESP32 boot and request a fresh Windows Wi-Fi scan; no firmware writes."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import subprocess
import sys
import time
import uuid
import serial

sys.stdout.reconfigure(errors="replace")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="COM12")
parser.add_argument("--reset", action="store_true")
parser.add_argument("--hold-open", action="store_true")
args = parser.parse_args()
port = serial.Serial(port=None, baudrate=115200, timeout=0.25)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    if args.reset:
        port.rts = True
        time.sleep(0.15)
        port.rts = False
    else:
        port.write(b"\nstatus\n")
    deadline = time.monotonic() + 7
    while time.monotonic() < deadline:
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(line, flush=True)
finally:
    if not args.hold_open:
        port.close()

wlan = C.WinDLL("wlanapi")
wlan.WlanOpenHandle.argtypes = [W.DWORD, C.c_void_p, C.POINTER(W.DWORD), C.POINTER(W.HANDLE)]
wlan.WlanScan.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
wlan.WlanCloseHandle.argtypes = [W.HANDLE, C.c_void_p]
handle, version = W.HANDLE(), W.DWORD()
result = wlan.WlanOpenHandle(2, None, C.byref(version), C.byref(handle))
if result:
    raise OSError(result, "WlanOpenHandle")
try:
    # Intel AX211 Wi-Fi interface, confirmed by netsh in this session.
    guid = C.create_string_buffer(uuid.UUID("80590500-d636-4a5d-85e0-e66b96959c20").bytes_le)
    result = wlan.WlanScan(handle, guid, None, None, None)
    print("Fresh scan requested, Windows result:", result, flush=True)
    if result:
        raise OSError(result, "WlanScan")
    time.sleep(8)
    listing = subprocess.run(["netsh", "wlan", "show", "networks", "mode=bssid"], capture_output=True, check=True)
    lines = listing.stdout.decode("mbcs", errors="replace").splitlines()
    found = False
    for index, line in enumerate(lines):
        if "Rail-ESP32" in line:
            print("\n".join(lines[index:index+12]))
            found = True
    if not found:
        print("Rail SSID absent from freshly scanned list")
finally:
    wlan.WlanCloseHandle(handle, None)
    port.close()
