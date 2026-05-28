"""E2E tests for Model Deploy Service.

Requires: model-deploy running at TEST_MODEL_DEPLOY_URL (default http://localhost:8091).
Run: pytest tests/model-deploy/test_end_to_end.py -v
"""

import hashlib
import io
import json
import os
import time

import pytest
import requests

BASE_URL = os.environ.get("TEST_MODEL_DEPLOY_URL", "http://localhost:8091")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class TestHealth:
    def test_health_ok(self):
        resp = requests.get(f"{BASE_URL}/api/v1/health", timeout=5)
        assert resp.status_code == 200
        body = resp.json()
        assert "db" in body
        assert "mqtt" in body
        assert body["db"] in ("ok", "error")
        assert body["mqtt"] in ("ok", "error")


class TestDeploy:
    def test_deploy_valid_model(self):
        content = b"mock onnx model binary content\x00\x01\x02\x03"
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/deploy",
            files={"model_file": ("autoencoder_1.0.0.onnx", io.BytesIO(content))},
            data={
                "model_name": "autoencoder",
                "version": "1.0.0",
                "metrics_json": json.dumps({"val_loss": 0.234, "threshold": 0.38}),
            },
            timeout=10,
        )
        assert resp.status_code == 201
        body = resp.json()
        assert body["model_name"] == "autoencoder"
        assert body["version"] == "1.0.0"
        assert body["status"] == "deployed"
        assert body["sha256"] == sha256_hex(content)

    def test_deploy_empty_model_name(self):
        content = b"mock onnx model"
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/deploy",
            files={"model_file": ("test.onnx", io.BytesIO(content))},
            data={"model_name": "", "version": "1.0.0"},
            timeout=10,
        )
        assert resp.status_code == 400

    def test_deploy_invalid_semver(self):
        content = b"mock onnx model"
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/deploy",
            files={"model_file": ("test.onnx", io.BytesIO(content))},
            data={"model_name": "autoencoder", "version": "not-semver"},
            timeout=10,
        )
        assert resp.status_code == 400

    def test_deploy_no_file(self):
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/deploy",
            data={"model_name": "autoencoder", "version": "1.0.0"},
            timeout=10,
        )
        assert resp.status_code == 400

    def test_deploy_multiple_versions(self):
        for version in ("1.1.0", "1.2.0", "1.3.0"):
            content = f"mock model v{version}".encode()
            resp = requests.post(
                f"{BASE_URL}/api/v1/models/deploy",
                files={"model_file": (f"autoencoder_{version}.onnx", io.BytesIO(content))},
                data={
                    "model_name": "autoencoder",
                    "version": version,
                    "metrics_json": json.dumps({"val_loss": 0.1, "version": version}),
                },
                timeout=10,
            )
            assert resp.status_code == 201, f"failed to deploy v{version}: {resp.text}"


class TestListModels:
    def test_list_models(self):
        resp = requests.get(f"{BASE_URL}/api/v1/models", timeout=5)
        assert resp.status_code == 200
        body = resp.json()
        assert "models" in body
        assert isinstance(body["models"], list)

    def test_list_models_not_empty(self):
        # Deploy if needed to populate
        content = b"list test model"
        requests.post(
            f"{BASE_URL}/api/v1/models/deploy",
            files={"model_file": ("list_test.onnx", io.BytesIO(content))},
            data={
                "model_name": "test_model",
                "version": "1.0.0",
            },
            timeout=10,
        )
        resp = requests.get(f"{BASE_URL}/api/v1/models", timeout=5)
        body = resp.json()
        assert len(body["models"]) > 0


class TestListVersions:
    def test_list_versions(self):
        resp = requests.get(f"{BASE_URL}/api/v1/models/autoencoder/versions", timeout=5)
        assert resp.status_code == 200
        body = resp.json()
        assert "versions" in body
        assert isinstance(body["versions"], list)

    def test_list_versions_nonexistent_model(self):
        resp = requests.get(f"{BASE_URL}/api/v1/models/nonexistent_model/versions", timeout=5)
        assert resp.status_code == 200
        body = resp.json()
        assert body["versions"] == []


class TestGetModel:
    def test_get_model(self):
        resp = requests.get(f"{BASE_URL}/api/v1/models/autoencoder", timeout=5)
        assert resp.status_code == 200
        body = resp.json()
        assert body["model_name"] == "autoencoder"
        assert "deployed" in body
        assert "versions" in body


class TestRollback:
    def test_rollback(self):
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/autoencoder/rollback",
            json={"version": "1.0.0"},
            timeout=10,
        )
        assert resp.status_code == 200
        body = resp.json()
        assert body["model_name"] == "autoencoder"
        assert body["status"] == "rolled_back"

    def test_rollback_nonexistent_version(self):
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/autoencoder/rollback",
            json={"version": "99.99.99"},
            timeout=10,
        )
        assert resp.status_code == 404

    def test_rollback_missing_version_field(self):
        resp = requests.post(
            f"{BASE_URL}/api/v1/models/autoencoder/rollback",
            json={},
            timeout=10,
        )
        assert resp.status_code == 400

    def test_rollback_empty_model_name(self):
        resp = requests.post(
            f"{BASE_URL}/api/v1/models//rollback",
            json={"version": "1.0.0"},
            timeout=10,
        )
        assert resp.status_code == 404  # chi routes /models/ to 404
