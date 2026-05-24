"""
End-to-end integration tests for rs232-gateway.

Uses socat virtual serial port pairs + pyserial + paho-mqtt to verify
the complete data pipeline: serial → parse → JSON → MQTT publish.

Run on Orange Pi 4 Pro only (requires socat, mosquitto, rs232-gateway binary).
"""

import subprocess
import time
import os
import signal
import json
import pytest

from conftest import (
    SERIAL_MASTER, SERIAL_SLAVE, make_frame, _crc16_modbus,
    write_test_config, GATEWAY_BIN, TEST_CONFIG
)


class TestEndToEnd:
    """End-to-end pipeline tests."""

    def test_system_status_frame(self, socat_process, mqtt_queue, gateway_process):
        """Send a system status frame, verify it appears on MQTT."""
        import serial

        # Build 8-byte system status payload
        payload = bytes([0, 1, 0, 0, 4, 0, 0, 0])

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.5)

        frame = make_frame(0x07, payload, seq=1)
        ser.write(frame)
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        found = [m for m in messages if m[1].get("cmd") == "system_status"]
        assert len(found) >= 1, f"No system_status message received. Got: {messages}"

        topic, payload = found[0]
        assert payload["source"] == "rs232"
        assert payload["dev_id"] == "de01"
        assert payload["data"]["system_state"] == 0
        assert payload["data"]["operation_mode"] == 1
        assert "ts" in payload

    def test_temp_humidity_frame(self, socat_process, mqtt_queue, gateway_process):
        """Send a temp/humidity frame, verify JSON values."""
        import serial
        import struct

        # Build 16-byte payload with known values
        payload = bytearray(16)
        struct.pack_into("<f", payload, 0, 25.5)   # temp_c
        struct.pack_into("<f", payload, 4, 60.0)   # humidity_rh
        struct.pack_into("<I", payload, 8, 12345)  # timestamp_ms
        payload[12] = 2   # SHT30
        payload[13] = 0   # normal
        # bytes 14-15 = 0 (raw_adc)

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.3)
        frame = make_frame(0x04, bytes(payload), seq=2)
        ser.write(frame)
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        found = [m for m in messages if m[1].get("cmd") == "temp_humidity"]
        assert len(found) >= 1, f"No temp_humidity message received. Got: {messages}"

        topic, payload = found[0]
        assert "de01/data/sensor" in topic
        assert payload["source"] == "rs232"
        assert abs(payload["data"]["temp_c"] - 25.5) < 0.1
        assert abs(payload["data"]["humidity_rh"] - 60.0) < 0.1

    def test_motor_status_frame(self, socat_process, mqtt_queue, gateway_process):
        """Send a motor status frame with known values."""
        import serial
        import struct

        payload = bytearray(26)
        struct.pack_into("<i", payload, 0, 1500)    # rpm
        struct.pack_into("<i", payload, 4, 3500)    # current_ma
        struct.pack_into("<i", payload, 8, 24000)   # bus_mv
        struct.pack_into("<i", payload, 12, 355)    # temp_dc (35.5 * 10)
        payload[16] = 1    # state
        payload[17] = 0    # fault
        struct.pack_into("<i", payload, 18, 75)     # duty
        payload[22] = 1    # direction (1=CW)
        payload[23] = 1    # pid_active

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.3)
        frame = make_frame(0x06, bytes(payload), seq=3)
        ser.write(frame)
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        found = [m for m in messages if m[1].get("cmd") == "motor_status"]
        assert len(found) >= 1, f"No motor_status message received. Got: {messages}"

        topic, json_msg = found[0]
        assert "de01/data/motor" in topic
        data = json_msg["data"]
        assert data["rpm"] == 1500
        assert data["current_ma"] == 3500
        assert data["bus_mv"] == 24000
        assert data["pid_active"] is True

    def test_corrupt_crc_not_published(self, socat_process, mqtt_queue, gateway_process):
        """Send a frame with corrupted CRC, verify it is NOT published."""
        import serial

        # Build a valid system_status frame first
        payload = bytes([0, 1, 0, 0, 4, 0, 0, 0])
        valid_frame = make_frame(0x07, payload, seq=10)
        # Corrupt the CRC byte
        corrupt_frame = bytearray(valid_frame)
        corrupt_frame[-3] ^= 0xFF  # Flip CRC high byte

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.3)
        ser.write(bytes(corrupt_frame))
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        # The corrupt frame should NOT produce a system_status message
        found = [m for m in messages if m[1].get("cmd") == "system_status"]
        assert len(found) == 0, f"Corrupt CRC frame should not be published, got: {found}"

    def test_self_sync_after_garbage(self, socat_process, mqtt_queue, gateway_process):
        """Send garbage bytes then a valid frame, verify self-sync."""
        import serial

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.3)

        # Send garbage
        garbage = bytes([0x00, 0xFF, 0x12, 0x34, 0x00, 0x00, 0x7F, 0x80])
        ser.write(garbage)
        time.sleep(0.2)

        # Send valid system status frame
        payload = bytes([0, 1, 0, 0, 4, 0, 0, 0])
        frame = make_frame(0x07, payload, seq=20)
        ser.write(frame)
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        found = [m for m in messages if m[1].get("cmd") == "system_status"]
        assert len(found) >= 1, "Valid frame after garbage should be received"

    def test_nde_feature_frame(self, socat_process, mqtt_queue, gateway_process):
        """Send NDE feature frame (100 bytes), verify 24-element float array."""
        import serial
        import struct

        payload = bytearray(100)
        payload[0] = 42  # window_idx
        # Fill 24 float values
        for i in range(24):
            struct.pack_into("<f", payload, 4 + i * 4, float(i) * 0.1)

        ser = serial.Serial(SERIAL_MASTER, 115200, timeout=0.5)
        time.sleep(0.3)
        frame = make_frame(0x17, bytes(payload), seq=5, dev_id=0x02)
        ser.write(frame)
        ser.close()
        time.sleep(1)

        messages = []
        while not mqtt_queue.empty():
            messages.append(mqtt_queue.get_nowait())

        found = [m for m in messages if m[1].get("cmd") == "nde_feature"]
        assert len(found) >= 1, f"No nde_feature message received. Got: {messages}"

        topic, json_msg = found[0]
        assert "nde01/data/sensor" in topic
        assert json_msg["dev_id"] == "nde01"
        assert json_msg["data"]["window_idx"] == 42
        assert len(json_msg["data"]["features"]) == 24


class TestErrorRecovery:
    """Error recovery and resilience tests."""

    def test_nonexistent_port_retry(self, socat_process):
        """Gateway should keep retrying when serial port doesn't exist."""
        write_test_config("/tmp/nonexistent_port_xyz")

        proc = subprocess.Popen(
            [GATEWAY_BIN, TEST_CONFIG],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        time.sleep(4)  # Should have retried at least once (2s delay)

        assert proc.poll() is None, "Gateway should not exit on missing port"

        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

        if os.path.exists(TEST_CONFIG):
            os.unlink(TEST_CONFIG)

