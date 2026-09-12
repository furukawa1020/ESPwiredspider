"""Provision ESP32 onto one saved Windows Wi-Fi profile without logging its key."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import serial

sys.stdout.reconfigure(errors="replace")
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--profile", required=True)
parser.add_argument("--port", default="COM12")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="rail-lan-") as folder:
    subprocess.run(["netsh", "wlan", "export", "profile", "name=" + args.profile,
                    "key=clear", "folder=" + folder], capture_output=True, check=True)
    files = list(Path(folder).glob("*.xml"))
    if len(files) != 1:
        raise SystemExit("Expected exactly one exported Wi-Fi profile")
    root = ET.parse(files[0]).getroot()
    ssid = root.findtext(".//{*}SSIDConfig/{*}SSID/{*}name")
    password = root.findtext(".//{*}sharedKey/{*}keyMaterial")
    protected = root.findtext(".//{*}sharedKey/{*}protected")
    if not ssid or not password or protected != "false":
        raise SystemExit("Saved Wi-Fi key was not available in usable form")
    payload = json.dumps({"op":"configure", "wifi_ssid":ssid, "wifi_password":password},
                         ensure_ascii=False, separators=(",", ":")).encode("utf-8")

port = serial.Serial(port=None, baudrate=115200, timeout=0.4, write_timeout=5)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    port.reset_input_buffer()
    port.write(payload + b"\n")
    port.flush()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        line = port.readline()
        if b"Configuration saved;" in line:
            print("Wi-Fi settings saved; ESP32 restarting stopped", flush=True)
            break
        if b"Configuration rejected" in line or b"save failed" in line:
            raise SystemExit("ESP32 rejected Wi-Fi configuration")
    else:
        raise SystemExit("No configuration save acknowledgement")
    deadline = time.monotonic() + 90
    next_query = 0
    last_report = 0
    while time.monotonic() < deadline:
        if time.monotonic() >= next_query:
            port.write(b"status\n")
            next_query = time.monotonic() + 1
        try:
            status = json.loads(port.readline())
        except (ValueError, UnicodeDecodeError):
            continue
        if status.get("type") != "status":
            continue
        if status.get("wifi_connected"):
            assert status["wifi_ssid"] == ssid, status
            assert status["gpio18"] == 0 and status["gpio19"] == 0, status
            Path(".pio/rail-lan-status.json").write_text(json.dumps(status, indent=2), encoding="utf-8")
            print(json.dumps({"wifi_ssid":ssid, "ip":status["sta_ip"], "url":"http://"+status["sta_ip"]+"/"}), flush=True)
            break
        if time.monotonic() - last_report > 10:
            print("Waiting for ESP32 to join the saved Wi-Fi", flush=True)
            last_report = time.monotonic()
    else:
        raise SystemExit("Wi-Fi settings saved, but ESP32 did not obtain a LAN address")
finally:
    port.close()
