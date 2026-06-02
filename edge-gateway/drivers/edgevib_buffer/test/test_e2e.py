#!/usr/bin/env python3
"""
test_e2e.py — EdgeVib RAM Buffer Block Device End-to-End Test

Verifies the full pipeline:
  1. Insert test data into /dev/edgevib-buffer
  2. Read back and verify header magic + payload integrity
  3. Check sysfs overrun_sectors counter
  4. Multi-sector write + read

Usage (on Orange Pi):
  python3 test_e2e.py
"""

import os
import struct
import sys
import hashlib

BLKDEV = "/dev/edgevib-buffer"
SYSFS_BASE = "/sys/devices/virtual/block/edgevib-buffer"
SECTOR_SIZE = 4096
MAGIC = 0x56424544  # "EDVB"

PASS = 0
FAIL = 0

def green(msg):
    global PASS
    PASS += 1
    print(f"  \033[32mPASS\033[0m: {msg}")

def red(msg):
    global FAIL
    FAIL += 1
    print(f"  \033[31mFAIL\033[0m: {msg}")

def test_write_read_roundtrip():
    """Write a sector with valid header, read back and verify"""
    print("\n--- Test: Write + Read with Structured Header ---")

    import json
    payload = json.dumps([{
        "device_id": "test01",
        "rms_x": 1.23,
        "overall_rms": 2.45
    }]).encode()

    # Build sector
    sector = bytearray(SECTOR_SIZE)
    # Header
    struct.pack_into('<I', sector, 0, MAGIC)           # magic
    crc = __import__('zlib').crc32(payload)
    struct.pack_into('<I', sector, 4, crc)              # crc32
    ts = int(__import__('time').time() * 1000)
    struct.pack_into('<Q', sector, 8, ts)               # timestamp_ms
    struct.pack_into('<I', sector, 16, len(payload))    # payload_len
    struct.pack_into('<I', sector, 20, 1)               # device_count
    struct.pack_into('<I', sector, 24, 42)              # batch_seq
    # Payload
    sector[32:32+len(payload)] = payload

    # Write
    try:
        with open(BLKDEV, 'wb') as f:
            f.write(sector)
        green(f"Wrote sector ({len(sector)} bytes)")
    except Exception as e:
        red(f"Write failed: {e}")
        return

    # Read back
    try:
        with open(BLKDEV, 'rb') as f:
            f.seek(0)
            readback = f.read(SECTOR_SIZE)

        read_magic = struct.unpack_from('<I', readback, 0)[0]
        read_crc = struct.unpack_from('<I', readback, 4)[0]
        read_payload_len = struct.unpack_from('<I', readback, 16)[0]

        if read_magic == MAGIC:
            green(f"Magic correct: 0x{read_magic:08X}")
        else:
            red(f"Magic mismatch: 0x{read_magic:08X}")

        if read_crc == crc:
            green(f"CRC32 correct: 0x{read_crc:08X}")
        else:
            red(f"CRC32 mismatch")

        if read_payload_len == len(payload):
            green(f"Payload length correct: {read_payload_len}")
        else:
            red(f"Payload length mismatch: {read_payload_len}")

        # Verify payload content
        read_payload = readback[32:32+read_payload_len]
        if read_payload == payload:
            green("Payload content verified")
        else:
            red("Payload content mismatch")

    except Exception as e:
        red(f"Read/verify failed: {e}")

def test_sysfs_counters():
    """Check sysfs attributes exist and are readable"""
    print("\n--- Test: sysfs Counters ---")

    for attr in ["overrun_sectors", "oldest_sector_age_ms"]:
        path = os.path.join(SYSFS_BASE, attr)
        try:
            with open(path) as f:
                val = f.read().strip()
            green(f"{attr} = {val}")
        except Exception as e:
            red(f"{attr}: {e}")

def test_multi_sector():
    """Write multiple sectors to exercise ring buffer"""
    print("\n--- Test: Multi-Sector Write ---")

    sector = bytearray(SECTOR_SIZE)
    struct.pack_into('<I', sector, 0, MAGIC)
    struct.pack_into('<I', sector, 24, 100)

    try:
        with open(BLKDEV, 'wb') as f:
            for i in range(10):
                struct.pack_into('<I', sector, 24, 100 + i)
                f.seek(i * SECTOR_SIZE)
                f.write(sector)
        green("Wrote 10 sectors successfully")

        # Read back sector 5
        with open(BLKDEV, 'rb') as f:
            f.seek(5 * SECTOR_SIZE)
            rb = f.read(SECTOR_SIZE)
            seq = struct.unpack_from('<I', rb, 24)[0]
        if seq == 105:
            green(f"Sector 5 batch_seq = {seq} (expected 105)")
        else:
            red(f"Sector 5 batch_seq = {seq}")

    except Exception as e:
        red(f"Multi-sector test failed: {e}")

def main():
    print("=" * 50)
    print(" EdgeVib RAM Buffer Block Device E2E Test")
    print("=" * 50)

    if not os.path.exists(BLKDEV):
        print(f"SKIP: {BLKDEV} not found — load edgevib_buffer.ko first")
        sys.exit(1)

    test_write_read_roundtrip()
    test_sysfs_counters()
    test_multi_sector()

    print(f"\n{'=' * 50}")
    print(f" Results: {PASS} passed, {FAIL} failed")
    print(f"{'=' * 50}")

    if FAIL > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
