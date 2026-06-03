#!/usr/bin/env python3
"""
test_e2e.py — EdgeVib GPIO End-to-End Integration Test (4 tests)

Tests the complete pipeline:
  MQTT → edgevib-gpio-d daemon → GPIO output → sysfs verification
  GPIO IRQ → sysfs inject → MQTT emergency topic

Requires:
  - Orange Pi 4 Pro with edgevib_gpio.ko loaded
  - edgevib-gpio-d daemon running
  - Mosquitto MQTT broker on localhost:1883
  - gpiod tools installed (apt install gpiod)

Usage:
  python3 test_e2e.py
"""

import json
import os
import subprocess
import sys
import time

import paho.mqtt.client as mqtt

# ---- Config ----
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
GPIO_CHIP = "/dev/gpiochip2"
ALERT_TOPIC = "EdgeVib/system/monitoring/alert"
INFERENCE_TOPIC = "EdgeVib/factory1/inference/de01/ai/report"
HEALTH_TOPIC = "EdgeVib/factory1/gateway/esp32-01/status/health"
ESTOP_TOPIC = "EdgeVib/system/emergency/estop"
PSU_TOPIC = "EdgeVib/system/emergency/psu_fail"

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

def gpio_get(chip, line):
    """Read a GPIO line value using gpioget."""
    result = subprocess.run(
        ["sudo", "gpioget", chip, str(line)],
        capture_output=True, text=True, timeout=5
    )
    return int(result.stdout.strip()) if result.returncode == 0 else None

def publish(topic, payload_dict):
    """Publish a JSON message to MQTT."""
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(MQTT_BROKER, MQTT_PORT, 10)
    client.loop_start()
    time.sleep(0.2)
    client.publish(topic, json.dumps(payload_dict), qos=1)
    time.sleep(0.5)
    client.loop_stop()
    client.disconnect()

def mqtt_listen(topic, timeout=3):
    """Listen for a message on an MQTT topic and return the payload."""
    received = []

    def on_message(client, userdata, msg):
        received.append(json.loads(msg.payload))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 10)
    client.subscribe(topic, qos=1)
    client.loop_start()

    start = time.time()
    while time.time() - start < timeout:
        if received:
            break
        time.sleep(0.1)

    client.loop_stop()
    client.disconnect()
    return received[0] if received else None


def main():
    print("=== EdgeVib GPIO E2E Test ===\n")

    # Check preconditions
    if not os.path.exists(GPIO_CHIP):
        red(f"GPIO chip {GPIO_CHIP} not found — is edgevib_gpio.ko loaded?")
        sys.exit(1)

    # Test 1: Prometheus CRITICAL alert → GPIO lines 0 and 1
    print("--- Test 1: MQTT CRITICAL alert → GPIO output ---")
    alert = {
        "status": "firing",
        "alerts": [{
            "labels": {"severity": "critical", "alertname": "TestCritical"},
            "annotations": {"summary": "Test critical alert"}
        }]
    }
    publish(ALERT_TOPIC, alert)
    time.sleep(1)

    line0 = gpio_get(GPIO_CHIP, 0)
    line1 = gpio_get(GPIO_CHIP, 1)
    if line0 == 0 and line1 == 1:
        green(f"CRITICAL: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 0,1)")
    else:
        red(f"CRITICAL: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 0,1)")

    # Test 2: Resolve alert → lines return to normal
    print("\n--- Test 2: MQTT resolve → GPIO returns to OK ---")
    alert["status"] = "resolved"
    publish(ALERT_TOPIC, alert)
    time.sleep(1)

    line0 = gpio_get(GPIO_CHIP, 0)
    line1 = gpio_get(GPIO_CHIP, 1)
    if line0 == 1 and line1 == 0:
        green(f"RESOLVED: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 1,0)")
    else:
        red(f"RESOLVED: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 1,0)")

    # Test 3: Inference WARNING → GATEWAY_ALERT high, SYSTEM_OK stays high
    print("\n--- Test 3: MQTT inference WARNING → GATEWAY_ALERT=1 ---")
    inf_report = {
        "site_id": "factory1",
        "device_id": "de01",
        "device_type": "motor",
        "severity": "WARNING"
    }
    publish(INFERENCE_TOPIC, inf_report)
    time.sleep(1)

    line0 = gpio_get(GPIO_CHIP, 0)
    line1 = gpio_get(GPIO_CHIP, 1)
    if line0 == 1 and line1 == 1:
        green(f"WARNING: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 1,1)")
    else:
        red(f"WARNING: SYSTEM_OK={line0}, GATEWAY_ALERT={line1} (expected 1,1)")

    # Test 4: GPIO IRQ → MQTT emergency topic
    print("\n--- Test 4: sysfs inject ESTOP IRQ → MQTT emergency topic ---")
    # Find sysfs inject_irq path
    sysfs_dir = "/sys/devices/virtual/edgevib-gpio/edgevib-gpio"
    if not os.path.exists(sysfs_dir):
        # Fallback: search
        result = subprocess.run(
            ["find", "/sys/devices", "-name", "edgevib-gpio", "-type", "d"],
            capture_output=True, text=True, timeout=3
        )
        sysfs_dir = result.stdout.strip().split("\n")[0] if result.stdout.strip() else ""

    if sysfs_dir and os.path.exists(f"{sysfs_dir}/inject_irq"):
        # Write inject_irq to trigger ESTOP falling edge
        subprocess.run(
            ["sudo", "tee", f"{sysfs_dir}/inject_irq"],
            input="4 0\n", capture_output=True, text=True, timeout=3
        )
        # Listen for the emergency MQTT topic
        msg = mqtt_listen(ESTOP_TOPIC, timeout=3)
        if msg and msg.get("action") == "shutdown":
            green(f"ESTOP IRQ → MQTT received: {msg}")
        else:
            red(f"ESTOP IRQ → MQTT: no message received on {ESTOP_TOPIC}")
    else:
        red(f"inject_irq sysfs not found at {sysfs_dir}/inject_irq")

    # Summary
    print(f"\n=== Results: {PASS} passed, {FAIL} failed ===")
    if FAIL > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
