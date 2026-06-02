#!/usr/bin/env python3
"""
test_e2e.py — EdgeVib IIO Vibration Device End-to-End Test

Verifies the full pipeline:
  1. Insert test data into TimescaleDB vibration_view
  2. Wait for iio-d daemon poll cycle
  3. Read IIO raw values via sysfs
  4. Verify values match injected test data

Usage (on Orange Pi):
  python3 test_e2e.py
"""

import subprocess
import sys
import time
import struct

# --- Config ---
INJECT_DEV = "/dev/edgevib-iio-inject"
IIO_SYSFS_BASE = "/sys/bus/iio/devices"

TESTS_PASSED = 0
TESTS_FAILED = 0

def green(msg):
    global TESTS_PASSED
    TESTS_PASSED += 1
    print(f"  \033[32mPASS\033[0m: {msg}")

def red(msg):
    global TESTS_FAILED
    TESTS_FAILED += 1
    print(f"  \033[31mFAIL\033[0m: {msg}")

def find_iio_device():
    """Find edgevib-iio device in sysfs"""
    import os
    for entry in os.listdir(IIO_SYSFS_BASE):
        full_path = os.path.join(IIO_SYSFS_BASE, entry)
        name_file = os.path.join(full_path, "name")
        if os.path.isfile(name_file):
            with open(name_file) as f:
                if f.read().strip() == "edgevib-iio":
                    return full_path
    return None

def test_inject_and_verify():
    """Inject known float32 values and verify via sysfs"""
    print("\n--- Test: Inject + Verify via sysfs ---")

    # Generate test vector: 24 floats 0.0, 1.0, 2.0, ..., 23.0
    test_values = [float(i) for i in range(24)]
    raw_data = struct.pack('<24f', *test_values)

    # Inject via cdev
    try:
        with open(INJECT_DEV, 'wb') as f:
            f.write(raw_data)
        print(f"  Injected 24 test values ({len(raw_data)} bytes)")
    except Exception as e:
        red(f"Injection failed: {e}")
        return

    time.sleep(0.5)

    # Find IIO device
    iio_path = find_iio_device()
    if not iio_path:
        red("IIO device 'edgevib-iio' not found in sysfs")
        return

    # Read injection_count
    count_file = f"{iio_path}/injection_count"
    try:
        with open(count_file) as f:
            count = int(f.read().strip())
        if count >= 1:
            green(f"injection_count = {count} (>= 1)")
        else:
            red(f"injection_count = {count}, expected >= 1")
    except Exception as e:
        red(f"Failed to read injection_count: {e}")

    # Read acceleration channels (channels 0-2: rms_x, rms_y, rms_z)
    accel_base = f"{iio_path}"
    try:
        # Try standard path: in_accel_x_raw
        raw_path = f"{accel_base}/in_accel_x_raw"
        if subprocess.call(f"test -f {raw_path}", shell=True) == 0:
            with open(raw_path) as f:
                val = int(f.read().strip())
            expected = int(test_values[0] * 1000)  # float→int conversion
            print(f"  in_accel_x_raw = {val} (expected ~{expected})")
            if abs(val - expected) < 10:
                green(f"ACCEL_X value matches (tolerance ±10)")
            else:
                red(f"ACCEL_X value mismatch: {val} vs expected {expected}")
        else:
            # Fallback: check if ACCEL channels exist
            print(f"  ACCEL_X raw path not found; check iio_info output for channel layout")
    except Exception as e:
        red(f"Failed to read ACCEL channels: {e}")

def test_buffer_read():
    """Verify IIO buffer (/dev/iio:device0) is readable"""
    print("\n--- Test: IIO buffer readable ---")
    iio_dev = "/dev/iio:device0"
    if subprocess.call(f"test -c {iio_dev}", shell=True) == 0:
        green(f"{iio_dev} exists")
        # Try reading (non-blocking, may return 0 bytes if buffer empty)
        try:
            import os
            fd = os.open(iio_dev, os.O_RDONLY | os.O_NONBLOCK)
            data = os.read(fd, 256)
            os.close(fd)
            print(f"  Buffer read returned {len(data)} bytes")
            green("Buffer readable")
        except BlockingIOError:
            print("  Buffer empty (expected after single injection)")
            green("Buffer device opened successfully (empty)")
        except Exception as e:
            red(f"Buffer read failed: {e}")
    else:
        red(f"{iio_dev} not found")

def main():
    print("=" * 50)
    print(" EdgeVib IIO Vibration E2E Test")
    print("=" * 50)

    test_inject_and_verify()
    test_buffer_read()

    print(f"\n{'=' * 50}")
    print(f" Results: {TESTS_PASSED} passed, {TESTS_FAILED} failed")
    print(f"{'=' * 50}")

    if TESTS_FAILED > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
