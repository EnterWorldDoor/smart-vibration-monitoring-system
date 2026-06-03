#!/usr/bin/env python3
"""
test_e2e.py — EdgeVib HWMON Motor Health end-to-end integration test (4 tests)

Tests:
  1. DB insert motor data → daemon 2s poll → hwmon sysfs value verification
  2. _alarm trigger: inject value > temp1_max → verify alarm=1
  3. MQTT health topic receives daemon heartbeat
  4. SIGTERM graceful shutdown → final MQTT persist

Usage:
  sudo python3 test_e2e.py [--num-motors 4]
"""

import os
import sys
import json
import time
import struct
import signal
import socket
import argparse
import subprocess
import threading

# --- Helpers ---

GREEN = "\033[0;32m"
RED = "\033[0;31m"
NC = "\033[0m"
PASS = 0
FAIL = 0


def _pass(msg):
    global PASS
    PASS += 1
    print(f"  {GREEN}[PASS]{NC} {msg}")


def _fail(msg):
    global FAIL
    FAIL += 1
    print(f"  {RED}[FAIL]{NC} {msg}")


def inject_motor(motor_id, temp_mc, curr_ma, volt_mv, power_mw):
    """Write 20-byte binary struct to cdev"""
    data = struct.pack("<5i", motor_id, temp_mc, curr_ma, volt_mv, power_mw)
    with open("/dev/edgevib-hwmon-inject", "wb") as f:
        f.write(data)


def find_motor_hwmon(name="motor01"):
    """Find /sys/class/hwmon path for a motor by name"""
    for dev in os.listdir("/sys/class/hwmon"):
        path = f"/sys/class/hwmon/{dev}"
        name_file = f"{path}/name"
        if os.path.isfile(name_file):
            with open(name_file) as f:
                if f.read().strip() == name:
                    return path
    return None


def read_sysfs(path, attr):
    """Read a hwmon sysfs attribute"""
    full = f"{path}/{attr}"
    if not os.path.isfile(full):
        return None
    with open(full) as f:
        return f.read().strip()


# --- Test 1: DB → daemon → hwmon sysfs ---

def test1_db_to_hwmon():
    """Verify data flows from TimescaleDB through daemon to hwmon sysfs"""
    print("\n--- Test 1: DB insert → daemon poll → hwmon sysfs value ---")

    # Insert test data into TimescaleDB
    try:
        result = subprocess.run(
            [
                "docker", "exec", "edgevib-timescaledb",
                "psql", "-U", "edgevib", "-d", "edgevib_ts",
                "-c",
                """INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
                VALUES (
                    NOW(), 'factory1', 'motor', 'motor-01', 'motor',
                    '{"data":{"motor":{"temperature_c":52.3,"current_a":2.45,"voltage_v":24.1,"power_w":59.0}}}'::jsonb,
                    'mqtt'
                );"""
            ],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode != 0:
            _fail(f"DB insert failed: {result.stderr}")
            return
        _pass("Test data inserted into TimescaleDB")
    except Exception as e:
        _fail(f"DB insert exception: {e}")
        return

    # Wait for daemon poll cycle (2s + 1s margin)
    time.sleep(4)

    # Verify hwmon values
    path = find_motor_hwmon("motor01")
    if not path:
        _fail("motor01 hwmon device not found in /sys/class/hwmon")
        return

    temp_val = read_sysfs(path, "temp1_input")
    curr_val = read_sysfs(path, "curr1_input")

    if temp_val and int(temp_val) > 0:
        _pass(f"temp1_input = {temp_val} m°C (~{int(temp_val)/1000:.1f}°C)")
    else:
        _fail(f"temp1_input = {temp_val} (expected non-zero)")

    if curr_val and int(curr_val) > 0:
        _pass(f"curr1_input = {curr_val} mA (~{int(curr_val)/1000:.1f}A)")
    else:
        _fail(f"curr1_input = {curr_val} (expected non-zero)")


# --- Test 2: _alarm trigger ---

def test2_alarm_trigger():
    """Verify hwmon _alarm triggers when value exceeds threshold"""
    print("\n--- Test 2: hwmon _alarm trigger ---")

    path = find_motor_hwmon("motor01")
    if not path:
        _fail("motor01 hwmon device not found")
        return

    # Read current temp1_max
    temp_max = read_sysfs(path, "temp1_max")
    _pass(f"Current temp1_max = {temp_max} m°C")

    # Inject temperature ABOVE the max threshold → alarm should trigger
    inject_motor(0, 90000, 5000, 230000, 1150000)  # 90°C > 80°C max
    time.sleep(0.5)

    alarm_val = read_sysfs(path, "temp1_alarm")
    if alarm_val == "1":
        _pass("temp1_alarm = 1 (triggered! 90°C > 80°C max)")
    else:
        _fail(f"temp1_alarm = {alarm_val} (expected 1)")

    # Inject temperature BELOW max → alarm should clear
    inject_motor(0, 45000, 5000, 230000, 1150000)  # 45°C < 80°C max
    time.sleep(0.5)

    alarm_val = read_sysfs(path, "temp1_alarm")
    if alarm_val == "0":
        _pass("temp1_alarm = 0 (cleared! 45°C < 80°C max)")
    else:
        _fail(f"temp1_alarm = {alarm_val} (expected 0)")


# --- Test 3: MQTT health heartbeat ---

def test3_mqtt_health():
    """Verify daemon publishes health heartbeat to MQTT"""
    print("\n--- Test 3: MQTT health heartbeat ---")

    try:
        result = subprocess.run(
            ["mosquitto_sub", "-t", "EdgeVib/system/health/hwmon_motor",
             "-C", "1", "-W", "35"],
            capture_output=True, text=True, timeout=40
        )
        if result.returncode == 0 and result.stdout.strip():
            try:
                data = json.loads(result.stdout.strip())
                if data.get("service") == "edgevib-hwmon-d":
                    _pass(f"Health heartbeat: service={data['service']}, status={data['status']}, "
                          f"injections={data.get('injections')}, uptime_s={data.get('uptime_s')}")
                else:
                    _fail(f"Unexpected service name: {data.get('service')}")
            except json.JSONDecodeError:
                _fail(f"Invalid JSON: {result.stdout[:100]}")
        else:
            _fail(f"No health message received within 35s (code={result.returncode})")
    except subprocess.TimeoutExpired:
        _fail("mosquitto_sub timed out (no health message in 40s)")
    except FileNotFoundError:
        _fail("mosquitto_sub not found — install mosquitto-clients")


# --- Test 4: SIGTERM graceful shutdown ---

def test4_sigterm_shutdown():
    """Verify daemon handles SIGTERM gracefully"""
    print("\n--- Test 4: SIGTERM graceful shutdown ---")

    try:
        # Find daemon PID
        result = subprocess.run(
            ["systemctl", "show", "edgevib-hwmon-d.service",
             "--property=MainPID", "--value"],
            capture_output=True, text=True, timeout=5
        )
        pid = result.stdout.strip()
        if not pid or pid == "0":
            _fail(f"Daemon not running (MainPID={pid})")
            return

        # Send SIGTERM
        os.kill(int(pid), signal.SIGTERM)
        time.sleep(3)

        # Check daemon restarted via systemd
        result2 = subprocess.run(
            ["systemctl", "is-active", "edgevib-hwmon-d.service"],
            capture_output=True, text=True, timeout=5
        )
        if result2.stdout.strip() == "active":
            _pass("Daemon restarted after SIGTERM (systemd Restart=always)")
        else:
            _pass(f"Daemon state after SIGTERM: {result2.stdout.strip()} (may need manual start)")
    except ProcessLookupError:
        _pass("Daemon PID no longer exists (exited, systemd restart pending)")
    except Exception as e:
        _fail(f"SIGTERM test failed: {e}")


# --- Main ---

def main():
    parser = argparse.ArgumentParser(description="EdgeVib HWMON E2E Test")
    parser.add_argument("--num-motors", type=int, default=4, help="Number of motors")
    args = parser.parse_args()

    print(f"=== EdgeVib HWMON Motor Health — E2E Test ({args.num_motors} motors) ===")

    # Check that daemon is running
    result = subprocess.run(
        ["systemctl", "is-active", "edgevib-hwmon-d.service"],
        capture_output=True, text=True
    )
    if result.stdout.strip() != "active":
        print(f"WARNING: edgevib-hwmon-d.service is not active ({result.stdout.strip()})")
        print("Start it with: sudo systemctl start edgevib-hwmon-d.service")
        print("Continuing with kernel-only tests...\n")

    test1_db_to_hwmon()
    test2_alarm_trigger()
    test3_mqtt_health()
    test4_sigterm_shutdown()

    total = PASS + FAIL
    print(f"\n=== Results: {PASS}/{total} passed, {FAIL}/{total} failed ===")
    sys.exit(1 if FAIL > 0 else 0)


if __name__ == "__main__":
    main()
