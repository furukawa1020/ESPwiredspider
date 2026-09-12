"""Temporarily connect to the ESP32 AP, exercise HTTP LED preview, restore Wi-Fi."""
import json
import sys
import argparse
import ctypes as C
from ctypes import wintypes as W
import uuid
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.request
import xml.etree.ElementTree as ET
import serial

BASE = "http://192.168.4.1"
sys.stdout.reconfigure(errors="replace")
INTERFACE = "Wi-Fi"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="COM11")
parser.add_argument("--restore-profile", required=True)
parser.add_argument("--reset", action="store_true")
parser.add_argument("--hold-open", action="store_true")
parser.add_argument("--soak-seconds", type=int, default=0)
parser.add_argument("--connect-timeout", type=int, default=90)
parser.add_argument("--close-during-soak", action="store_true")
parser.add_argument("--motor", action="store_true", help="Run two 1-second physical L9110S motor pulses")
parser.add_argument("--varied", action="store_true", help="Add short pulses and an early-stop check to the motor demo")
parser.add_argument("--status-only", action="store_true", help="Only check the network and stopped state; do not send moves")
parser.add_argument("--reconnects", type=int, default=0)
args = parser.parse_args()
ORIGINAL_PROFILE = args.restore_profile
PROFILE = "Codex-Rail-LED-Demo"
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def netsh(*args):
    result = subprocess.run(["netsh", "wlan", *args], capture_output=True, check=True)
    print(result.stdout.decode("mbcs", errors="replace"), flush=True)
    return result


def request(path, data=None):
    payload = None if data is None else json.dumps(data).encode()
    req = urllib.request.Request(BASE + path, data=payload,
                                 headers={"Content-Type": "application/json"})
    with opener.open(req, timeout=3) as response:
        return response.status, json.load(response)


port = serial.Serial(port=None, baudrate=115200, timeout=0.5, write_timeout=3)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    if args.reset:
        port.rts = True
        time.sleep(0.15)
        port.rts = False
        time.sleep(3)
    port.reset_input_buffer()
    port.write(b"\naccess\n")
    deadline = time.monotonic() + 8
    access = None
    while time.monotonic() < deadline:
        try:
            value = json.loads(port.readline())
        except (ValueError, UnicodeDecodeError):
            continue
        if "ssid" in value and "password" in value:
            access = value
            break
    if access is None:
        raise RuntimeError("ESP32 did not provide AP credentials")
finally:
    if not args.hold_open:
        port.close()

ns = "http://www.microsoft.com/networking/WLAN/profile/v1"
ET.register_namespace("", ns)
root = ET.Element("{" + ns + "}WLANProfile")


def element(parent, tag, text=None):
    node = ET.SubElement(parent, "{" + ns + "}" + tag)
    node.text = text
    return node


element(root, "name", PROFILE)
ssid_config = element(root, "SSIDConfig")
ssid = element(ssid_config, "SSID")
element(ssid, "name", access["ssid"])
element(root, "connectionType", "ESS")
element(root, "connectionMode", "auto")
element(root, "autoSwitch", "false")
security = element(element(root, "MSM"), "security")
auth = element(security, "authEncryption")
element(auth, "authentication", "WPA2PSK")
element(auth, "encryption", "AES")
element(auth, "useOneX", "false")
key = element(security, "sharedKey")
element(key, "keyType", "passPhrase")
element(key, "protected", "false")
element(key, "keyMaterial", access["password"])
report = []
added = False
try:
    with tempfile.TemporaryDirectory(prefix="rail-led-") as folder:
        profile_file = Path(folder) / "profile.xml"
        ET.ElementTree(root).write(profile_file, encoding="utf-8", xml_declaration=True)
        netsh("add", "profile", "filename=" + str(profile_file), "user=current")
        added = True
    netsh("set", "profileorder", "name=" + PROFILE, "interface=" + INTERFACE, "priority=1")
    wlan = C.WinDLL("wlanapi")
    wlan.WlanOpenHandle.argtypes = [W.DWORD, C.c_void_p, C.POINTER(W.DWORD), C.POINTER(W.HANDLE)]
    wlan.WlanScan.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]
    wlan.WlanCloseHandle.argtypes = [W.HANDLE, C.c_void_p]
    handle, version = W.HANDLE(), W.DWORD()
    if wlan.WlanOpenHandle(2, None, C.byref(version), C.byref(handle)) == 0:
        try:
            guid = C.create_string_buffer(uuid.UUID("80590500-d636-4a5d-85e0-e66b96959c20").bytes_le)
            print("Fresh WLAN scan result:", wlan.WlanScan(handle, guid, None, None, None), flush=True)
            time.sleep(8)
        finally:
            wlan.WlanCloseHandle(handle, None)
    netsh("disconnect", "interface=" + INTERFACE)
    time.sleep(1)
    netsh("connect", "name=" + PROFILE, "ssid=" + access["ssid"], "interface=" + INTERFACE)
    time.sleep(4)
    netsh("show", "interfaces")
    print(subprocess.run(["ipconfig"], capture_output=True).stdout.decode("mbcs", errors="replace"), flush=True)
    deadline = time.monotonic() + args.connect_timeout
    attempts = 0
    consecutive = 0
    while True:
        if time.monotonic() >= deadline:
            raise RuntimeError("Timed out waiting for consecutive HTTP responses")
        try:
            code, status = request("/api/v1/rail/status")
            expected_mode = "dc_l9110s" if args.motor else "led_preview"
            assert status["mode"] == expected_mode, "Firmware mode does not match requested demo"
            assert status["rgb_led_ready"] and not status["http_auth_required"]
            consecutive += 1
            if consecutive >= 3:
                break
            time.sleep(0.2)
        except (OSError, ValueError):
            consecutive = 0
            attempts += 1
            if attempts % 5 == 0:
                print("Waiting for DHCP/HTTP, attempt:", attempts, flush=True)
            if time.monotonic() >= deadline:
                print(subprocess.run(["ipconfig"], capture_output=True).stdout.decode("mbcs", errors="replace"), flush=True)
                raise RuntimeError("Could not reach ESP32 HTTP server")
            time.sleep(0.5)
    print("Connected; unauthenticated GET status succeeded", flush=True)
    if status.get("network_revision", 0) >= 2:
        with opener.open(BASE + "/", timeout=3) as response:
            assert response.status == 200 and b"/api/v1/rail" in response.read()
        preflight = urllib.request.Request(BASE + "/api/v1/rail/move", method="OPTIONS", headers={
            "Origin":"http://example.test", "Access-Control-Request-Method":"POST", "Access-Control-Request-Headers":"content-type"})
        with opener.open(preflight, timeout=3) as response:
            assert response.status == 204 and response.headers["Access-Control-Allow-Origin"] == "*"
        print("PASS: control page and browser CORS preflight", flush=True)
    cases = [("x", 1, [40, 0, 0]), ("x", -1, [40, 0, 0])] if args.motor else [
        ("x", 1, [40, 0, 0]), ("y", 1, [0, 40, 0]), ("z", 1, [0, 0, 40]), ("z", -1, None)]
    duration_ms = 1000 if args.motor else 3000
    if args.motor and args.varied:
        cases += [("x", 1, [40, 0, 0]), ("x", -1, [40, 0, 0])]
    if args.status_only:
        cases = []
    for case_index, (axis, direction, expected) in enumerate(cases):
        if args.motor and args.varied and case_index >= 2:
            duration_ms = 500
        code, result = request("/api/v1/rail/move", {"axis": axis, "direction": direction, "duration_ms": duration_ms})
        assert code == 202 and result["status"] == "accepted", result
        start = time.monotonic()
        colors = set()
        pin_states = set()
        while time.monotonic() - start < duration_ms / 1000 + 0.5:
            _, status = request("/api/v1/rail/status")
            colors.add(tuple(status["led_rgb"]))
            if args.motor:
                pin_states.add((status["gpio18"], status["gpio19"]))
            assert not status["standby_pin_high"], status
            time.sleep(0.1)
        assert not any(a["active"] or a["pending"] for a in status["axes"]), status
        assert tuple(expected or [0, 0, 40]) in colors, colors
        assert (0, 0, 0) in colors, colors
        if args.motor:
            assert ((1, 0) if direction == 1 else (0, 1)) in pin_states, pin_states
            assert (status["gpio18"], status["gpio19"]) == (0, 0), status
        entry = {"axis": axis, "direction": direction, "accepted": True,
                 "duration_ms": duration_ms,
                 "observed_rgb": sorted(colors), "timed_stop": True}
        report.append(entry)
        if args.motor:
            entry["gpio18_gpio19"] = sorted(pin_states)
        print(json.dumps(entry), flush=True)
        if args.motor:
            time.sleep(1)
    if args.motor and args.varied and not args.status_only:
        code, result = request("/api/v1/rail/move", {"axis":"x", "direction":1, "duration_ms":3000})
        assert code == 202, result
        time.sleep(0.5)
        _, before_stop = request("/api/v1/rail/status")
        assert before_stop["axes"][0]["active"], before_stop
        code, result = request("/api/v1/rail/stop", {"axis":"x"})
        assert code == 202, result
        time.sleep(0.1)
        _, after_stop = request("/api/v1/rail/status")
        assert not after_stop["axes"][0]["active"] and not after_stop["axes"][0]["pending"], after_stop
        assert (after_stop["gpio18"],after_stop["gpio19"]) == (0,0), after_stop
        entry = {"early_stop":True, "remaining_ms_before_stop":before_stop["axes"][0]["remaining_ms"], "gpio18_gpio19":[0,0]}
        report.append(entry)
        print(json.dumps(entry), flush=True)
    code, stopped = request("/api/v1/rail/stop", {"axis": None})
    assert code == 202, stopped
    print("PASS: HTTP status/stop" if args.status_only else "PASS: HTTP move/status/stop without authentication; RGB outputs and timed stop", flush=True)
    if args.close_during_soak:
        port.close()
        print("USB serial closed; checking Wi-Fi-only operation", flush=True)
    end = time.monotonic() + args.soak_seconds
    checked = 0
    baseline_stops = status.get("ap_stops")
    previous_uptime = status.get("uptime_ms", 0)
    while time.monotonic() < end:
        code, status = request("/api/v1/rail/status")
        assert code == 200 and not status["standby_pin_high"]
        assert status.get("ap_stops") == baseline_stops, "AP stopped during stability check"
        assert status.get("uptime_ms", 0) >= previous_uptime, "ESP32 restarted during stability check"
        previous_uptime = status.get("uptime_ms", 0)
        checked += 1
        if checked % 10 == 0:
            print("HTTP stability checks:", checked, flush=True)
        time.sleep(1)
    print("PASS: stability duration", args.soak_seconds, "seconds; successful requests", checked, flush=True)
    report.append({"stability_seconds":args.soak_seconds, "successful_requests":checked,
                   "serial_closed":not port.is_open, "ap_stops":baseline_stops})
    for cycle in range(args.reconnects):
        netsh("disconnect", "interface=" + INTERFACE)
        time.sleep(1)
        netsh("connect", "name=" + PROFILE, "ssid=" + access["ssid"], "interface=" + INTERFACE)
        reconnect_deadline = time.monotonic() + args.connect_timeout
        successes = 0
        while successes < 5:
            if time.monotonic() > reconnect_deadline:
                raise RuntimeError("HTTP did not recover after Wi-Fi reconnection")
            try:
                code, status = request("/api/v1/rail/status")
                assert code == 200 and status["gpio18"] == 0 and status["gpio19"] == 0
                successes += 1
            except OSError:
                successes = 0
            time.sleep(0.5)
        print("PASS: Wi-Fi reconnection", cycle + 1, flush=True)
        report.append({"reconnection":cycle+1, "consecutive_successful_requests":successes})
finally:
    try:
        request("/api/v1/rail/stop", {"axis": None})
    except Exception:
        pass
    netsh("connect", "name=" + ORIGINAL_PROFILE, "interface=" + INTERFACE)
    if added:
        netsh("delete", "profile", "name=" + PROFILE, "interface=" + INTERFACE)
    print("Original Wi-Fi reconnection requested", flush=True)
    report_file = ".pio/motor-http-demo.json" if args.motor else ".pio/led-http-demo.json"
    Path(report_file).write_text(json.dumps(report, indent=2), encoding="utf-8")
    port.close()
