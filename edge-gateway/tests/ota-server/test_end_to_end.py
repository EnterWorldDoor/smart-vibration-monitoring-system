"""
OTA Server end-to-end integration tests.

Requires: Running ota-server Docker container on Orange Pi with
TimescaleDB + Mosquitto accessible.

Usage:
  cd tests/ota-server
  pip install -r ../integration/requirements-test.txt paho-mqtt
  TEST_OTA_URL=http://192.168.1.1:8090 pytest test_end_to_end.py -v
"""

import os
import io
import hashlib
import time
import json
import pytest
import requests

OTA_URL = os.environ.get("TEST_OTA_URL", "http://localhost:8090")


def sha256_hex(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


class TestOTAHealth:
    """Verify ota-server health endpoint."""

    def test_health_ok(self):
        resp = requests.get(f"{OTA_URL}/api/v1/health", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert "db" in data
        assert "mqtt" in data


class TestFirmwareUpload:
    """Verify firmware upload API."""

    def test_upload_empty_version_json(self):
        resp = requests.get(f"{OTA_URL}/firmware/version.json", timeout=5)
        assert resp.status_code == 200

    def test_upload_valid_firmware(self):
        content = b"fake firmware binary content for testing"
        files = {"file": ("esp32-gateway-1.0.0.bin", io.BytesIO(content), "application/octet-stream")}
        form = {
            "platform": "esp32",
            "version": "1.0.0",
            "build_date": "2026-05-28",
            "release_notes": "Integration test firmware",
        }
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", files=files, data=form, timeout=10)
        assert resp.status_code == 201
        data = resp.json()
        assert data["platform"] == "esp32"
        assert data["version"] == "1.0.0"
        assert data["sha256"] == sha256_hex(content)

    def test_upload_duplicate_version(self):
        content = b"duplicate firmware"
        files = {"file": ("esp32-gateway-1.0.0.bin", io.BytesIO(content), "application/octet-stream")}
        form = {
            "platform": "esp32",
            "version": "1.0.0",
            "build_date": "2026-05-28",
        }
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", files=files, data=form, timeout=10)
        # Duplicate should succeed (upsert)
        assert resp.status_code == 201

    def test_upload_invalid_platform(self):
        content = b"bad"
        files = {"file": ("test.bin", io.BytesIO(content), "application/octet-stream")}
        form = {"platform": "invalid", "version": "1.0.0", "build_date": "2026-05-28"}
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", files=files, data=form, timeout=10)
        assert resp.status_code == 400

    def test_upload_invalid_version(self):
        content = b"bad"
        files = {"file": ("test.bin", io.BytesIO(content), "application/octet-stream")}
        form = {"platform": "esp32", "version": "not-semver", "build_date": "2026-05-28"}
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", files=files, data=form, timeout=10)
        assert resp.status_code == 400

    def test_upload_missing_file(self):
        form = {"platform": "esp32", "version": "2.0.0", "build_date": "2026-05-28"}
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", data=form, timeout=10)
        assert resp.status_code == 400

    def test_upload_missing_build_date(self):
        content = b"missing date"
        files = {"file": ("test.bin", io.BytesIO(content), "application/octet-stream")}
        form = {"platform": "esp32", "version": "2.0.0"}
        resp = requests.post(f"{OTA_URL}/api/v1/firmware/upload", files=files, data=form, timeout=10)
        assert resp.status_code == 400


class TestVersionJSON:
    """Verify version.json dynamic generation."""

    def test_version_json_has_esp32(self):
        resp = requests.get(f"{OTA_URL}/firmware/version.json", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert "esp32" in data
        assert data["esp32"]["latest_version"] == "1.0.0"
        assert data["esp32"]["file"] == "esp32-gateway-1.0.0.bin"

    def test_version_json_cache_control(self):
        resp = requests.get(f"{OTA_URL}/firmware/version.json", timeout=5)
        assert resp.headers.get("Cache-Control") == "no-cache"


class TestFirmwareDownload:
    """Verify firmware binary download."""

    def test_download_existing_file(self):
        resp = requests.get(f"{OTA_URL}/firmware/esp32/esp32-gateway-1.0.0.bin", timeout=10)
        assert resp.status_code == 200
        assert len(resp.content) > 0
        assert sha256_hex(resp.content) == sha256_hex(b"duplicate firmware")

    def test_download_nonexistent_file(self):
        resp = requests.get(f"{OTA_URL}/firmware/esp32/nonexistent.bin", timeout=5)
        assert resp.status_code == 404

    def test_download_invalid_platform(self):
        resp = requests.get(f"{OTA_URL}/firmware/bad/test.bin", timeout=5)
        assert resp.status_code == 400

    def test_download_path_traversal(self):
        resp = requests.get(f"{OTA_URL}/firmware/esp32/../../../etc/passwd", timeout=5)
        assert resp.status_code == 400


class TestVersionsAPI:
    """Verify versions list API."""

    def test_list_all_versions(self):
        resp = requests.get(f"{OTA_URL}/api/v1/firmware/versions", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert "versions" in data
        assert len(data["versions"]) >= 1

    def test_list_esp32_versions(self):
        resp = requests.get(f"{OTA_URL}/api/v1/firmware/versions?platform=esp32", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        for v in data["versions"]:
            assert v["platform"] == "esp32"

    def test_list_empty_platform(self):
        resp = requests.get(f"{OTA_URL}/api/v1/firmware/versions?platform=f407", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert data["versions"] == []


class TestUpgradeHistory:
    """Verify upgrade history API."""

    def test_list_empty_history(self):
        resp = requests.get(f"{OTA_URL}/api/v1/firmware/upgrade-history", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert "history" in data

    def test_history_pagination(self):
        resp = requests.get(f"{OTA_URL}/api/v1/firmware/upgrade-history?limit=5&offset=0", timeout=5)
        assert resp.status_code == 200
        data = resp.json()
        assert len(data["history"]) <= 5


class TestTriggerUpgrade:
    """Verify manual upgrade trigger."""

    def test_trigger_missing_fields(self):
        resp = requests.post(
            f"{OTA_URL}/api/v1/firmware/trigger-upgrade",
            json={"platform": "esp32"},
            timeout=5,
        )
        assert resp.status_code == 400

    def test_trigger_bad_version(self):
        resp = requests.post(
            f"{OTA_URL}/api/v1/firmware/trigger-upgrade",
            json={"platform": "esp32", "device_id": "esp32-01", "target_version": "99.99.99"},
            timeout=5,
        )
        assert resp.status_code == 400
