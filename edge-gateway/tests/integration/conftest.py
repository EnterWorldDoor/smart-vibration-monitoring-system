"""Shared fixtures for EdgeVib Dashboard integration tests."""
import json
import os
import time
from datetime import datetime, timedelta, timezone

import psycopg2
import pytest
import requests

# ---------------------------------------------------------------------------
# Config from environment (with sensible defaults for local/Docker testing)
# ---------------------------------------------------------------------------
DB_CONFIG = {
    "host": os.environ.get("TEST_DB_HOST", "localhost"),
    "port": int(os.environ.get("TEST_DB_PORT", "5432")),
    "dbname": os.environ.get("TEST_DB_NAME", "edgevib_ts"),
    "user": os.environ.get("TEST_DB_USER", "edgevib"),
    "password": os.environ.get("TEST_DB_PASSWORD", "edgevib123"),
}

GRAFANA_URL = os.environ.get("TEST_GRAFANA_URL", "http://localhost:3000")
GRAFANA_AUTH = (
    os.environ.get("TEST_GRAFANA_USER", "admin"),
    os.environ.get("TEST_GRAFANA_PASSWORD", "admin123"),
)

DASHBOARD_UIDS = [
    "edgevib-device-overview",
    "edgevib-vibration-detail",
    "edgevib-ai-diagnosis",
    "edgevib-dual-channel",
    "edgevib-system-health",
    "edgevib-environment",
]

INSERT_SQL = """
INSERT INTO sensor_data (time, site_id, device_type, device_id, data_type, payload, source_path)
VALUES (%s, %s, %s, %s, %s, %s, 'mqtt')
"""


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def db_conn():
    """Return a psycopg2 connection to TimescaleDB."""
    conn = psycopg2.connect(**DB_CONFIG)
    conn.autocommit = True
    yield conn
    conn.close()


@pytest.fixture(scope="session")
def grafana_session():
    """Return an authenticated requests Session for Grafana API."""
    session = requests.Session()
    session.auth = GRAFANA_AUTH
    for _ in range(30):
        try:
            resp = session.get(f"{GRAFANA_URL}/api/health", timeout=2)
            if resp.status_code == 200:
                break
        except requests.ConnectionError:
            pass
        time.sleep(1)
    return session


@pytest.fixture(autouse=True)
def clean_test_data(db_conn):
    """Remove test data before and after each test."""
    cursor = db_conn.cursor()
    cursor.execute(
        "DELETE FROM sensor_data WHERE site_id = 'test_site' AND source_path = 'mqtt'"
    )
    db_conn.commit()
    yield
    cursor.execute(
        "DELETE FROM sensor_data WHERE site_id = 'test_site' AND source_path = 'mqtt'"
    )
    db_conn.commit()


# ---------------------------------------------------------------------------
# Test data helpers
# ---------------------------------------------------------------------------

def make_payload(**overrides):
    """Build a realistic ESP32 JSONB payload with optional field overrides.

    By default, produces a complete payload with all sections. Pass keyword
    args to override any field. Pass a section as None to remove it entirely.

    Section overrides accept a dict to merge into that section (e.g.
    vibration={'overall_rms': 5.0}) or None to delete the section.
    """
    base = {
        "dev_id": 1,
        "timestamp_ms": 1712345678000,
        "mode": "upload",
        "service_state": "RUNNING",
        "data_quality": 2,
        "samples_analyzed": 2048,
        "total_analyses": 100,
        "temperature_valid": "true",
        "data": {
            "vibration": {
                "rms_x": 1.23,
                "rms_y": 0.87,
                "rms_z": 2.15,
                "overall_rms": 2.61,
                "peak_freq": 142.5,
                "peak_amp": 0.034,
            },
            "environment": {
                "temperature_c": 32.5,
                "humidity_rh": 65.2,
            },
            "compensation": {
                "active": "true",
                "offset_x": 0.01,
                "offset_y": 0.02,
                "offset_z": 0.01,
            },
            "fft_peaks": [
                {"freq": 50.0, "amp": 0.12},
                {"freq": 142.5, "amp": 0.034},
                {"freq": 285.0, "amp": 0.015},
            ],
            "ai": {
                "class_id": 0,
                "class_name": "normal",
                "confidence": 0.92,
                "cascade_source": "primary_cnn",
                "inference_time_us": 15000,
            },
            "dual_channel": {
                "rms_ratio": 1.15,
                "spectral_similarity": 0.88,
                "phase_coherence": 0.73,
                "nde_online": 1,
                "nde_errors": 0,
            },
        },
    }

    # Apply section-level overrides and top-level overrides
    for key, value in overrides.items():
        if key in base["data"]:
            if value is None:
                del base["data"][key]
            elif isinstance(value, dict):
                base["data"][key].update(value)
        elif key in base:
            if key == "service_state":
                base[key] = value
            else:
                base[key] = value

    return json.dumps(base)


def insert_test_record(cursor, time_offset_sec=0, site_id="test_site",
                        device_type="motor", device_id="de01",
                        data_type="sensor", payload_str=None):
    """Insert a single test record and return the payload dict."""
    if payload_str is None:
        payload_str = make_payload()
    t = datetime.now(timezone.utc) - timedelta(seconds=time_offset_sec)
    cursor.execute(INSERT_SQL, (t, site_id, device_type, device_id, data_type, payload_str))
    return json.loads(payload_str)
