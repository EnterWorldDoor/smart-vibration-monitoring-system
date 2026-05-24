"""
Integration test fixtures for rs232-gateway.

Requires:
  - socat (for virtual serial port pairs)
  - Mosquitto MQTT broker running on localhost:1883
  - rs232-gateway binary built at ../build/rs232-gateway
"""

import pytest
import subprocess
import time
import os
import signal
import json
import threading
import queue

SERIAL_MASTER = "/tmp/rs232test_master"
SERIAL_SLAVE = "/tmp/rs232test_slave"
GATEWAY_BIN = os.path.join(os.path.dirname(__file__), "..", "..", "build", "rs232-gateway")
TEST_CONFIG = os.path.join(os.path.dirname(__file__), "test_config.yaml")


def write_test_config(port):
    """Write a minimal config YAML pointing at the virtual serial port."""
    config = f"""serial:
  port: "{port}"
  baudrate: 115200
  data_bits: 8
  stop_bits: 1
  parity: "none"
  timeout_ms: 100

protocol:
  header: 0xAA55
  tail: 0x0D
  crc: "CRC16-MODBUS"

mqtt:
  broker_url: "tcp://localhost:1883"
  client_id: "test-rs232-gateway"
  publish_topic_prefix: "EdgeVib/factory1/motor"

heartbeat:
  peer_timeout_ms: 3000
"""
    with open(TEST_CONFIG, "w") as f:
        f.write(config)


@pytest.fixture(scope="module")
def gateway_process(socat_process):
    """Start the rs232-gateway binary pointed at the virtual serial port."""
    write_test_config(SERIAL_SLAVE)

    # Kill any leftover test instance
    subprocess.run(["pkill", "-f", "test-rs232-gateway"], capture_output=True)
    time.sleep(0.3)

    proc = subprocess.Popen(
        [GATEWAY_BIN, TEST_CONFIG],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(2)  # Wait for startup + MQTT connect

    # Verify process is alive
    assert proc.poll() is None, \
        f"Gateway exited early: stderr={proc.stderr.read().decode()}"

    yield proc

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    if os.path.exists(TEST_CONFIG):
        os.unlink(TEST_CONFIG)


@pytest.fixture(scope="session")
def socat_process():
    """Create a virtual serial port pair using socat."""
    # Kill any leftover instances
    subprocess.run(["pkill", "-f", f"socat.*{SERIAL_MASTER}"],
                   capture_output=True)
    time.sleep(0.5)
    # Remove stale device nodes
    for p in [SERIAL_MASTER, SERIAL_SLAVE]:
        if os.path.exists(p):
            os.unlink(p)

    proc = subprocess.Popen(
        ["socat", f"PTY,link={SERIAL_MASTER},raw,echo=0,mode=666",
         f"PTY,link={SERIAL_SLAVE},raw,echo=0,mode=666"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    time.sleep(1)  # Wait for socat to create the PTYs

    if not os.path.exists(SERIAL_MASTER) or not os.path.exists(SERIAL_SLAVE):
        proc.terminate()
        proc.wait()
        pytest.skip("socat failed to create virtual serial ports (is socat installed?)")

    yield proc

    proc.terminate()
    proc.wait()
    for p in [SERIAL_MASTER, SERIAL_SLAVE]:
        if os.path.exists(p):
            os.unlink(p)


@pytest.fixture
def mqtt_queue():
    """MQTT message collector that returns received messages as a queue."""
    import paho.mqtt.client as mqtt

    q = queue.Queue()

    def on_connect(client, userdata, flags, rc):
        client.subscribe("EdgeVib/#", qos=1)

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
            q.put((msg.topic, payload))
        except (json.JSONDecodeError, UnicodeDecodeError):
            q.put((msg.topic, {"_raw": msg.payload.hex()}))

    client = mqtt.Client(client_id="test-rs232-integration")
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect("localhost", 1883, 60)
    client.loop_start()
    time.sleep(0.5)  # Wait for connection + subscription

    yield q

    client.loop_stop()
    client.disconnect()


def make_frame(cmd, payload, seq=1, dev_id=0x01):
    """
    Build a binary protocol frame matching the STM32 firmware format.

    Frame: [AA 55] [LEN_H LEN_L] [DEV] [CMD] [SEQ] [DATA...] [CRC_H CRC_L] [0D]
    CRC16-MODBUS over [LEN_H LEN_L DEV CMD SEQ DATA...]
    """
    import struct

    payload_len = len(payload)
    frame = bytearray()

    # Header
    frame.append(0xAA)
    frame.append(0x55)
    # Length (big-endian)
    frame.append((payload_len >> 8) & 0xFF)
    frame.append(payload_len & 0xFF)
    # Device ID
    frame.append(dev_id)
    # Command
    frame.append(cmd)
    # Sequence
    frame.append(seq)
    # Data
    frame.extend(payload)

    # CRC16-MODBUS over bytes [2] through end of data
    crc = _crc16_modbus(frame[2:])
    frame.append((crc >> 8) & 0xFF)
    frame.append(crc & 0xFF)
    # Tail
    frame.append(0x0D)

    return bytes(frame)


def _crc16_modbus(data):
    """CRC16-MODBUS: polynomial 0x8005 (reflected 0xA001), init 0xFFFF."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF
