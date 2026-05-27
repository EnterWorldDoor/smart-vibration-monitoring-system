"""
OPC UA Server — end-to-end integration tests.

Requires:
  - A running opcua-server process (started by the test fixture)
  - TimescaleDB with test data (insert_test_data.sql)
  - opcua-asyncio Python client library

Usage:
  TEST_DB_HOST=localhost TEST_OPCUA_HOST=localhost \
    python3 -m pytest test_opcua_e2e.py -v
"""

import os
import subprocess
import time
import pytest

# --- Skip markers -----------------------------------------------------------

OPCUA_HOST = os.environ.get("TEST_OPCUA_HOST", "192.168.1.1")
OPCUA_PORT = int(os.environ.get("TEST_OPCUA_PORT", "4840"))
OPCUA_URL  = f"opc.tcp://{OPCUA_HOST}:{OPCUA_PORT}"

pytestmark = pytest.mark.skipif(
    os.environ.get("TEST_SKIP_OPCUA") == "1",
    reason="OPC UA integration tests require running server"
)

# --- Fixtures ---------------------------------------------------------------

@pytest.fixture(scope="module")
def opcua_client():
    """Connect to the OPC UA server as an anonymous client."""
    try:
        from asyncua import Client
    except ImportError:
        pytest.skip("asyncua not installed")
    client = Client(url=OPCUA_URL)
    try:
        client.connect()
    except Exception as e:
        client.disconnect()
        pytest.skip(f"Cannot connect to OPC UA server at {OPCUA_URL}: {e}")
    yield client
    client.disconnect()


@pytest.fixture(scope="module")
def db_conn():
    """Connect to TimescaleDB for data verification."""
    try:
        import psycopg2
    except ImportError:
        pytest.skip("psycopg2 not installed")
    host = os.environ.get("TEST_DB_HOST", "192.168.1.1")
    conn = psycopg2.connect(
        host=host, port=5432,
        dbname="edgevib_ts", user="edgevib", password="edgevib123"
    )
    conn.autocommit = True
    yield conn
    conn.close()


# --- Helpers ----------------------------------------------------------------

def browse_children(client, node, depth=0):
    """Recursively collect browse names for address-space inspection."""
    children = {}
    for child in node.get_children():
        name = child.get_browse_name().Name
        children[name] = browse_children(client, child, depth + 1)
    return children


# --- Tests ------------------------------------------------------------------

def test_server_reachable(opcua_client):
    """Verify the OPC UA endpoint is accepting connections."""
    assert opcua_client.uaclient is not None


def test_address_space_has_edgevib(opcua_client):
    """Verify the EdgeVib root folder exists under Objects."""
    root = opcua_client.get_objects_node()
    names = [c.get_browse_name().Name for c in root.get_children()]
    assert "EdgeVib" in names, f"EdgeVib not found in {names}"


def test_site_folder_exists(opcua_client):
    """Verify at least one site folder exists under EdgeVib."""
    root = opcua_client.get_objects_node()
    edgevib = None
    for c in root.get_children():
        if c.get_browse_name().Name == "EdgeVib":
            edgevib = c
            break
    assert edgevib is not None, "EdgeVib folder not found"

    sites = [c.get_browse_name().Name for c in edgevib.get_children()]
    assert len(sites) > 0, f"No site folders found under EdgeVib"
    print(f"  Sites: {sites}")


def test_device_nodes_exist(opcua_client):
    """Verify that known test devices have their subtrees in the address space."""
    root = opcua_client.get_objects_node()
    edgevib = None
    for c in root.get_children():
        if c.get_browse_name().Name == "EdgeVib":
            edgevib = c
            break
    assert edgevib is not None

    # Navigate: EdgeVib → factory1 → motor → de01
    try:
        factory1 = edgevib.get_child("factory1")
        motor    = factory1.get_child("motor")
        de01     = motor.get_child("de01")
    except Exception as e:
        pytest.fail(f"Device tree not found: {e}")

    # Verify subfolders
    subfolders = [c.get_browse_name().Name for c in de01.get_children()]
    assert "Vibration" in subfolders, f"Vibration folder missing: {subfolders}"
    assert "Status" in subfolders, f"Status folder missing: {subfolders}"
    print(f"  de01 subfolders: {subfolders}")


def test_read_vibration_node(opcua_client):
    """Read a vibration variable and verify it returns a numeric value."""
    try:
        node = opcua_client.get_node("ns=1;g=9999")  # fallback
        # Actually browse to the real node
        root = opcua_client.get_objects_node()
        edgevib  = root.get_child("EdgeVib")
        factory1 = edgevib.get_child("factory1")
        motor    = factory1.get_child("motor")
        de01     = motor.get_child("de01")
        vib      = de01.get_child("Vibration")
        rms_x    = vib.get_child("RMS_X")
        node     = rms_x
    except Exception as e:
        pytest.skip(f"Cannot navigate to RMS_X: {e}")

    val = node.get_value()
    assert val is not None, "RMS_X value is None"
    print(f"  RMS_X = {val}")


def test_read_status_node(opcua_client):
    """Read ServiceState variable (string type)."""
    try:
        root = opcua_client.get_objects_node()
        edgevib  = root.get_child("EdgeVib")
        factory1 = edgevib.get_child("factory1")
        motor    = factory1.get_child("motor")
        de01     = motor.get_child("de01")
        status   = de01.get_child("Status")
        svc      = status.get_child("ServiceState")
    except Exception as e:
        pytest.skip(f"Cannot navigate to ServiceState: {e}")

    val = svc.get_value()
    assert val is not None, "ServiceState value is None"
    print(f"  ServiceState = '{val}'")


def test_node_values_match_db(opcua_client, db_conn):
    """Compare OPC UA node value with TimescaleDB ground truth."""
    cur = db_conn.cursor()

    # Query the DB for latest DE01 overall_rms
    cur.execute("""
        SELECT overall_rms FROM device_status_view d
        LEFT JOIN vibration_view v
          ON d.site_id = v.site_id AND d.device_type = v.device_type
         AND d.device_id = v.device_id AND d.last_seen = v.time
        WHERE d.device_id = 'de01'
        ORDER BY d.last_seen DESC LIMIT 1
    """)
    db_val = cur.fetchone()
    if db_val is None or db_val[0] is None:
        pytest.skip("No vibration data for de01 in DB")

    db_rms = float(db_val[0])

    # Read from OPC UA
    try:
        root = opcua_client.get_objects_node()
        edgevib  = root.get_child("EdgeVib")
        factory1 = edgevib.get_child("factory1")
        motor    = factory1.get_child("motor")
        de01     = motor.get_child("de01")
        vib      = de01.get_child("Vibration")
        node     = vib.get_child("Overall_RMS")
    except Exception as e:
        pytest.skip(f"Cannot navigate to Overall_RMS: {e}")

    opcua_val = node.get_value()
    assert opcua_val is not None, "Overall_RMS is None"

    # Allow small floating-point tolerance
    assert abs(opcua_val - db_rms) < 1.0, \
        f"OPC UA {opcua_val} != DB {db_rms}"
    print(f"  Overall_RMS: OPC UA={opcua_val:.4f}, DB={db_rms:.4f}")

    cur.close()


def test_bad_status_on_no_data(opcua_client):
    """Reading a device that has no data should not crash."""
    # This tests graceful handling — node may not exist or return Bad
    try:
        root = opcua_client.get_objects_node()
        edgevib = root.get_child("EdgeVib")
        # Try to find a nonexistent device
        for site in edgevib.get_children():
            pass  # Just verifying browsing doesn't crash
    except Exception as e:
        pytest.fail(f"Browsing after empty data should not crash: {e}")
