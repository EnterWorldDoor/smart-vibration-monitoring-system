#!/usr/bin/python3
"""
test_e2e.py — EdgeVib V4L2 Virtual Video end-to-end integration tests
Run on Orange Pi 4 Pro: sudo python3 test/test_e2e.py
Requires: edgevib_video.ko loaded, TimescaleDB accessible, Go daemon running
"""
import os
import sys
import struct
import subprocess
import time

INJECT_DEV = "/dev/edgevib-video-inject"
FRAME_ATTR = "/sys/devices/virtual/edgevib-video-inject/edgevib-video-inject/frame_count"


def test_inject_binary_frame():
    """Test 1: Write a binary YUYV frame via cdev and verify frame_count increments."""
    w, h = 640, 480
    frame_sz = w * h * 2
    hdr = struct.pack('<5i', 0, w, h, 0x56595559, frame_sz)
    # Create a test pattern: alternating black/white pixels
    yuyv = b''
    for y in range(h):
        for x in range(w // 2):
            if (x + y) % 2 == 0:
                yuyv += b'\x10\x80\x10\x80'  # dark
            else:
                yuyv += b'\xEB\x80\xEB\x80'  # bright
    payload = hdr + yuyv

    before = int(open(FRAME_ATTR).read().strip())
    with open(INJECT_DEV, 'wb') as f:
        n = os.write(f.fileno(), payload)
    assert n == len(payload), f"short write: {n}/{len(payload)}"
    after = int(open(FRAME_ATTR).read().strip())
    assert after > before, f"frame_count did not increment: {before} -> {after}"
    print("[PASS] test_inject_binary_frame")


def test_inject_to_device_1():
    """Test 2: Inject to second device (dev_id=1)."""
    w, h = 1920, 1080
    frame_sz = w * h * 2
    hdr = struct.pack('<5i', 1, w, h, 0x56595559, frame_sz)
    yuyv = b'\x80\x80\x80\x80' * (w * h // 2)
    payload = hdr + yuyv

    with open(INJECT_DEV, 'wb') as f:
        n = os.write(f.fileno(), payload)
    assert n == len(payload), f"short write: {n}/{len(payload)}"
    print("[PASS] test_inject_to_device_1")


def test_invalid_dimensions():
    """Test 3: Invalid data_size should be rejected."""
    hdr = struct.pack('<5i', 0, 640, 480, 0x56595559, 100)  # wrong data_size
    with open(INJECT_DEV, 'wb') as f:
        try:
            os.write(f.fileno(), hdr + b'x' * 100)
            assert False, "should have raised OSError"
        except OSError:
            pass  # expected — EINVAL
    print("[PASS] test_invalid_dimensions")


def test_invalid_device_id():
    """Test 4: Out-of-range device_id should be rejected."""
    hdr = struct.pack('<5i', 99, 640, 480, 0x56595559, 640*480*2)
    with open(INJECT_DEV, 'wb') as f:
        try:
            os.write(f.fileno(), hdr + b'x' * 640 * 480 * 2)
            assert False, "should have raised OSError"
        except OSError:
            pass  # expected — EINVAL
    print("[PASS] test_invalid_device_id")


def test_v4l2_format():
    """Test 5: V4L2 device exposes correct format."""
    # Find the edgevib_video device
    result = subprocess.run(
        ["v4l2-ctl", "--list-devices"], capture_output=True, text=True, timeout=10)
    lines = result.stdout.split('\n')
    dev_path = None
    for i, line in enumerate(lines):
        if "EdgeVib Virtual Cam" in line:
            # Next non-empty line is the /dev/videoX path
            for j in range(i+1, min(i+5, len(lines))):
                if '/dev/video' in lines[j]:
                    dev_path = lines[j].strip()
                    break
            break

    if dev_path is None:
        print("[SKIP] test_v4l2_format — no device found (daemon state)")
        return

    fmt_result = subprocess.run(
        ["v4l2-ctl", "-d", dev_path, "--list-formats"],
        capture_output=True, text=True, timeout=10)
    assert "YUYV" in fmt_result.stdout, f"YUYV not in formats: {fmt_result.stdout}"
    print(f"[PASS] test_v4l2_format ({dev_path} -> YUYV)")


if __name__ == "__main__":
    # Ensure cdev exists
    if not os.path.exists(INJECT_DEV):
        print(f"SKIP: {INJECT_DEV} not found. Is edgevib_video.ko loaded?")
        sys.exit(0)

    test_inject_binary_frame()
    test_inject_to_device_1()
    test_invalid_dimensions()
    test_invalid_device_id()
    test_v4l2_format()

    print("\n=== ALL E2E TESTS PASSED ===")
