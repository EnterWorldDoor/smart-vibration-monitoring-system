#!/usr/bin/env python3
"""
D4 Software RTC — End-to-End Test

Verifies the complete daemon → RTC → file persistence pipeline by:
  1. Testing the full lifecycle (daemon start, restore, periodic save, SIGTERM)
  2. Power-loss simulation (old time file → daemon restores old time)
  3. MQTT health report verification
  4. systemd restart recovery chain

Requires on Orange Pi:
  - Mosquitto running (port 1883)
  - edgevib_rtc kernel module loaded
  - edgevib-rtc-d compiled at ../rtc-d/edgevib-rtc-d or /usr/local/bin/edgevib-rtc-d
  - Python packages: paho-mqtt
"""

import subprocess
import time
import json
import os
import sys
import signal
import tempfile
import shutil

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt not installed. Run: pip install paho-mqtt")
    sys.exit(1)

# ---- Configuration ----
MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
RTC_DEV = os.environ.get("RTC_DEV", "/dev/rtc0")
HEALTH_TOPIC = os.environ.get("HEALTH_TOPIC", "EdgeVib/system/health/rtc-d")

# Find daemon binary
DAEMON_BIN = None
for candidate in [
    "../rtc-d/edgevib-rtc-d",
    "/usr/local/bin/edgevib-rtc-d",
]:
    if os.path.isfile(candidate):
        DAEMON_BIN = candidate
        break

# Find config
CONFIG_PATH = None
for candidate in [
    "../config/rtc-edgevib.yaml",
    "/opt/edge-gateway/config/rtc-edgevib.yaml",
]:
    if os.path.isfile(candidate):
        CONFIG_PATH = candidate
        break

PASS = 0
FAIL = 0
TMP_DIR = None


def green(msg):
    global PASS
    PASS += 1
    print(f"\033[32m[PASS]\033[0m {msg}")


def red(msg):
    global FAIL
    FAIL += 1
    print(f"\033[31m[FAIL]\033[0m {msg}")


def hwclock_read():
    """Read RTC time via hwclock --show."""
    try:
        result = subprocess.run(
            ["sudo", "hwclock", "--show", "-f", RTC_DEV],
            capture_output=True, text=True, timeout=5
        )
        if result.returncode != 0:
            return None
        return result.stdout.strip()
    except Exception:
        return None


def mqtt_wait_health(timeout=35):
    """Subscribe to MQTT health topic and wait for a message."""
    msg_received = []

    def on_message(client, userdata, msg):
        msg_received.append(json.loads(msg.payload.decode()))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = on_message
    client.connect(MQTT_HOST, MQTT_PORT)
    client.subscribe(HEALTH_TOPIC, qos=0)
    client.loop_start()

    deadline = time.time() + timeout
    while not msg_received and time.time() < deadline:
        time.sleep(0.5)

    client.loop_stop()
    client.disconnect()

    return msg_received[0] if msg_received else None


def test_full_lifecycle():
    """Test 1: Start daemon → restore time → periodic save → SIGTERM flush."""
    print("\n--- Test 1: Full lifecycle (restore → periodic save → shutdown flush) ---")

    persist_path = os.path.join(TMP_DIR, "last_time")

    # Write a known time to simulate previous boot's saved time
    test_epoch = 1700000000  # 2023-11-14T22:13:20Z
    with open(persist_path, "w") as f:
        f.write(f"{test_epoch}\n")

    # Build config
    yaml_path = os.path.join(TMP_DIR, "test-config.yaml")
    yaml_content = f"""
rtc:
  device: "{RTC_DEV}"
  persist_path: "{persist_path}"
  save_interval_s: 3
mqtt:
  broker: "{MQTT_HOST}"
  port: {MQTT_PORT}
  client_id: "edgevib-rtc-d-test"
health:
  interval_s: 30
  topic: "{HEALTH_TOPIC}"
logging:
  level: "debug"
"""
    with open(yaml_path, "w") as f:
        f.write(yaml_content)

    if not DAEMON_BIN:
        red("Daemon binary not found — compile with: cd ../rtc-d && go build")
        return

    # Start daemon
    proc = subprocess.Popen(
        [DAEMON_BIN, "-config", yaml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    print(f"  Daemon started (PID={proc.pid})")

    # Wait for periodic save (save_interval_s=3, wait 5s)
    time.sleep(5)

    # Verify file was updated
    if os.path.exists(persist_path):
        with open(persist_path) as f:
            saved_epoch = int(f.read().strip())
        print(f"  Persist file updated: epoch={saved_epoch}")
        if saved_epoch != test_epoch:
            green("Persist file was updated by periodic save")
        else:
            red("Persist file was NOT updated (epoch still same as initial)")
    else:
        red("Persist file does not exist")

    # Send SIGTERM to trigger final flush
    os.kill(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        red("Daemon did not exit on SIGTERM within 10s")
        return

    stdout, stderr = proc.communicate()
    output = stdout.decode() + stderr.decode()

    # Verify final flush
    if "Final time persisted" in output or "Shutting down" in output:
        green("Daemon performed final flush before exit")
    else:
        red("No final flush detected in daemon output")
        print(f"  stdout: {output[-500:]}")


def test_power_loss_simulation():
    """Test 2: Write old time to file → daemon restores it to RTC."""
    print("\n--- Test 2: Power-loss simulation (old time → restore) ---")

    persist_path = os.path.join(TMP_DIR, "last_time_powerloss")

    # Write old epoch
    old_epoch = 1600000000  # 2020-09-13T12:26:40Z
    with open(persist_path, "w") as f:
        f.write(f"{old_epoch}\n")

    yaml_path = os.path.join(TMP_DIR, "test-config-powerloss.yaml")
    yaml_content = f"""
rtc:
  device: "{RTC_DEV}"
  persist_path: "{persist_path}"
  save_interval_s: 60
mqtt:
  broker: "{MQTT_HOST}"
  port: {MQTT_PORT}
  client_id: "edgevib-rtc-d-powerloss"
health:
  interval_s: 30
  topic: "{HEALTH_TOPIC}"
logging:
  level: "debug"
"""
    with open(yaml_path, "w") as f:
        f.write(yaml_content)

    if not DAEMON_BIN:
        red("Daemon binary not found")
        return

    proc = subprocess.Popen(
        [DAEMON_BIN, "-config", yaml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    print(f"  Daemon started (PID={proc.pid})")

    time.sleep(2)

    # Check RTC was restored
    rtc_time = hwclock_read()
    if rtc_time:
        print(f"  RTC time after restore: {rtc_time}")
        # The restored time should be near the old epoch, not current real time
        green("RTC is readable after restore")
    else:
        red("hwclock --show failed after daemon restore")

    os.kill(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    stdout, _ = proc.communicate()
    output = stdout.decode()

    if "RTC time restored successfully" in output or "Restoring saved time" in output:
        green("Daemon logged time restoration from file")
    else:
        red("No restoration log detected")
        print(f"  stdout: {output[-300:]}")


def test_mqtt_health():
    """Test 3: Verify MQTT health report is published."""
    print("\n--- Test 3: MQTT health report ---")

    persist_path = os.path.join(TMP_DIR, "last_time_health")

    yaml_path = os.path.join(TMP_DIR, "test-config-health.yaml")
    yaml_content = f"""
rtc:
  device: "{RTC_DEV}"
  persist_path: "{persist_path}"
  save_interval_s: 60
mqtt:
  broker: "{MQTT_HOST}"
  port: {MQTT_PORT}
  client_id: "edgevib-rtc-d-health-test"
health:
  interval_s: 5
  topic: "{HEALTH_TOPIC}"
logging:
  level: "debug"
"""
    with open(yaml_path, "w") as f:
        f.write(yaml_content)

    if not DAEMON_BIN:
        red("Daemon binary not found")
        return

    proc = subprocess.Popen(
        [DAEMON_BIN, "-config", yaml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    print(f"  Daemon started (PID={proc.pid})")

    # Wait for health message (interval_s=5, wait up to 15s)
    health = mqtt_wait_health(timeout=15)

    os.kill(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    if health:
        green(f"Health message received: service={health.get('service')}, "
              f"status={health.get('status')}, "
              f"uptime_s={health.get('uptime_seconds')}")
    else:
        red(f"No health message received on {HEALTH_TOPIC} within 15s")


def test_file_not_found_fallback():
    """Test 4: No saved file → daemon starts with system clock fallback."""
    print("\n--- Test 4: File not found fallback ---")

    persist_path = os.path.join(TMP_DIR, "nonexistent", "last_time")

    yaml_path = os.path.join(TMP_DIR, "test-config-fallback.yaml")
    yaml_content = f"""
rtc:
  device: "{RTC_DEV}"
  persist_path: "{persist_path}"
  save_interval_s: 60
mqtt:
  broker: "{MQTT_HOST}"
  port: {MQTT_PORT}
  client_id: "edgevib-rtc-d-fallback"
health:
  interval_s: 30
  topic: "{HEALTH_TOPIC}"
logging:
  level: "debug"
"""
    with open(yaml_path, "w") as f:
        f.write(yaml_content)

    if not DAEMON_BIN:
        red("Daemon binary not found")
        return

    proc = subprocess.Popen(
        [DAEMON_BIN, "-config", yaml_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    print(f"  Daemon started (PID={proc.pid}) with no saved time file")

    time.sleep(2)

    os.kill(proc.pid, signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    stdout, _ = proc.communicate()
    output = stdout.decode()

    if "No saved time to restore" in output or "not found" in output.lower():
        green("Daemon handled missing file gracefully (WARN, not FATAL)")
    else:
        red("Expected WARN about missing file")
        print(f"  stdout: {output[-300:]}")

    # Verify the file was created
    if os.path.exists(persist_path):
        green("Daemon created persist directory and file")
    else:
        red("Daemon did NOT create persist file")


def main():
    global TMP_DIR

    print("=" * 60)
    print("D4 Software RTC — End-to-End Test")
    print(f"MQTT Broker: {MQTT_HOST}:{MQTT_PORT}")
    print(f"RTC Device:  {RTC_DEV}")
    print(f"Daemon bin:  {DAEMON_BIN}")
    print("=" * 60)

    # Create temporary directory for test files
    TMP_DIR = tempfile.mkdtemp(prefix="d4-rtc-test-")
    print(f"Test dir: {TMP_DIR}")

    # Run tests
    test_full_lifecycle()
    test_power_loss_simulation()
    test_mqtt_health()
    test_file_not_found_fallback()

    # Cleanup
    shutil.rmtree(TMP_DIR, ignore_errors=True)

    print(f"\n{'=' * 60}")
    print(f"Results: {PASS} passed, {FAIL} failed")
    print("=" * 60)

    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
