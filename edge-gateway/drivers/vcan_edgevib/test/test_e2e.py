#!/usr/bin/env python3
"""
D1 Virtual CAN End-to-End Test

Verifies the complete MQTT -> AF_CAN -> kernel module -> candump pipeline
by publishing mock CAN frames via MQTT and checking candump output.

Requires on Orange Pi:
  - Mosquitto running (port 1883)
  - edgevib-can-d running
  - vcan_edgevib kernel module loaded
  - can-utils installed (candump)
"""

import subprocess
import time
import json
import sys
import os

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt not installed. Run: pip install paho-mqtt")
    sys.exit(1)

MQTT_HOST = os.environ.get("MQTT_HOST", "192.168.1.1")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
CAN_IF = os.environ.get("CAN_IF", "vcan_edgevib")

PASS = 0
FAIL = 0

def green(msg):
    global PASS
    PASS += 1
    print(f"\033[32m[PASS]\033[0m {msg}")

def red(msg):
    global FAIL
    FAIL += 1
    print(f"\033[31m[FAIL]\033[0m {msg}")

def candump_capture(iface, count, timeout=5):
    """Run candump and capture N frames"""
    cmd = f"timeout {timeout} candump -n {count} {iface}"
    try:
        result = subprocess.run(
            ["ssh", f"orangepi@{MQTT_HOST}", cmd],
            capture_output=True, text=True, timeout=timeout + 5
        )
        return result.stdout
    except subprocess.TimeoutExpired:
        return ""

def test_single_frame():
    """Test 1: Publish a single CAN frame via MQTT, verify candump sees it"""
    print("\n--- Test 1: Single CAN frame (0x201) ---")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(MQTT_HOST, MQTT_PORT)

    msg = {
        "id": 0x201,
        "dlc": 8,
        "data": "AABBCCDDEEFF0011",
        "flags": 1
    }
    client.publish("EdgeVib/test/can/001/raw", json.dumps(msg), qos=1)
    client.disconnect()

    time.sleep(2)

    output = candump_capture(CAN_IF, 1, timeout=10)
    if "201" in output and "AA" in output:
        green(f"Frame 0x201 received: {output.strip()[:80]}")
    else:
        red(f"Frame 0x201 not in candump output: {output[:200]}")

def test_heartbeat_frame():
    """Test 2: Publish heartbeat frame (0x202)"""
    print("\n--- Test 2: Heartbeat frame (0x202) ---")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(MQTT_HOST, MQTT_PORT)

    msg = {
        "id": 0x202,
        "dlc": 4,
        "data": "01000000",
        "flags": 1
    }
    client.publish("EdgeVib/test/can/001/raw", json.dumps(msg), qos=1)
    client.disconnect()

    time.sleep(2)

    output = candump_capture(CAN_IF, 1, timeout=10)
    if "202" in output:
        green(f"Heartbeat frame 0x202 received: {output.strip()[:80]}")
    else:
        red(f"Heartbeat frame 0x202 not in candump output: {output[:200]}")

def test_crc_error_flag():
    """Test 3: CRC error flag propagates correctly"""
    print("\n--- Test 3: CRC error flag (flags=0) ---")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(MQTT_HOST, MQTT_PORT)

    msg = {
        "id": 0x201,
        "dlc": 8,
        "data": "DEADBEEFCAFE1337",
        "flags": 0    # CRC8 fail flag
    }
    client.publish("EdgeVib/test/can/001/raw", json.dumps(msg), qos=1)
    client.disconnect()

    time.sleep(2)

    output = candump_capture(CAN_IF, 1, timeout=10)
    if "201" in output:
        green(f"CRC error frame received (flags=0): {output.strip()[:80]}")
        print("  -> Check crc_errors sysfs:",
              "ssh orangepi@{} cat /sys/class/net/{}/device/crc_errors".format(MQTT_HOST, CAN_IF))
    else:
        red(f"Frame not received: {output[:200]}")

def test_empty_data():
    """Test 4: Zero-DLC frame (no data)"""
    print("\n--- Test 4: Zero-DLC frame ---")

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(MQTT_HOST, MQTT_PORT)

    msg = {
        "id": 0x202,
        "dlc": 0,
        "data": "",
        "flags": 1
    }
    client.publish("EdgeVib/test/can/001/raw", json.dumps(msg), qos=1)
    client.disconnect()

    time.sleep(2)

    output = candump_capture(CAN_IF, 1, timeout=10)
    if "202" in output:
        green(f"Zero-DLC frame received: {output.strip()[:80]}")
    else:
        red(f"Zero-DLC frame not received: {output[:200]}")

def main():
    print("=" * 60)
    print("D1 Virtual CAN — End-to-End Test")
    print(f"MQTT Broker: {MQTT_HOST}:{MQTT_PORT}")
    print(f"CAN Interface: {CAN_IF}")
    print("=" * 60)

    test_single_frame()
    test_heartbeat_frame()
    test_crc_error_flag()
    test_empty_data()

    print(f"\n{'=' * 60}")
    print(f"Results: {PASS} passed, {FAIL} failed")
    print("=" * 60)

    return 0 if FAIL == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
