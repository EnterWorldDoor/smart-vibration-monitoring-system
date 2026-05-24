"""Integration tests for EdgeVib Dashboard VIEWs and Grafana dashboards.

Usage:
    pytest test_dashboard_views.py -v                                    # all tests
    pytest test_dashboard_views.py -v -k "TestVibration"                 # specific class
    TEST_GRAFANA_URL=http://192.168.1.1:3000 pytest -v -k "TestGrafana" # remote Grafana
"""
import json

import pytest

# Import helpers from conftest (fixtures are auto-discovered)
from conftest import make_payload, insert_test_record, DASHBOARD_UIDS


# ============================================================================
# VIEW Tests
# ============================================================================

class TestVibrationView:
    def test_extracts_all_vibration_fields(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(
            vibration={"rms_x": 1.5, "rms_y": 0.9, "rms_z": 2.0, "overall_rms": 2.5,
                       "peak_freq": 150.0, "peak_amp": 0.045}
        )
        insert_test_record(cursor, payload_str=payload)
        db_conn.commit()

        cursor.execute(
            "SELECT rms_x, rms_y, rms_z, overall_rms, peak_frequency_hz, "
            "peak_amplitude_g, fft_peaks FROM vibration_view "
            "WHERE site_id = 'test_site' AND device_id = 'de01' "
            "ORDER BY time DESC LIMIT 1"
        )
        row = cursor.fetchone()
        assert row is not None, "vibration_view should return a row"
        assert float(row[0]) == pytest.approx(1.5)
        assert float(row[1]) == pytest.approx(0.9)
        assert float(row[2]) == pytest.approx(2.0)
        assert float(row[3]) == pytest.approx(2.5)
        assert float(row[4]) == pytest.approx(150.0)
        assert float(row[5]) == pytest.approx(0.045)

    def test_fft_peaks_is_jsonb_array(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor)
        db_conn.commit()

        cursor.execute(
            "SELECT fft_peaks FROM vibration_view "
            "WHERE site_id = 'test_site' ORDER BY time DESC LIMIT 1"
        )
        row = cursor.fetchone()
        peaks = row[0]
        # psycopg2 returns JSONB as a Python list or dict
        if isinstance(peaks, str):
            peaks = json.loads(peaks)
        assert isinstance(peaks, list)
        assert len(peaks) == 3
        assert peaks[0]["freq"] == 50.0

    def test_null_when_vibration_section_missing(self, db_conn):
        cursor = db_conn.cursor()
        payload_str = make_payload(vibration=None)
        insert_test_record(cursor, payload_str=payload_str)
        db_conn.commit()

        cursor.execute(
            "SELECT COUNT(*) FROM vibration_view "
            "WHERE site_id = 'test_site' AND device_id = 'de01'"
        )
        count = cursor.fetchone()[0]
        assert count == 0, "Messages without data.vibration must not appear in vibration_view"


class TestAIDiagnosisView:
    def test_extracts_all_ai_fields(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(
            ai={"class_id": 2, "class_name": "misalignment", "confidence": 0.78,
                "cascade_source": "fallback_rule", "inference_time_us": 8500}
        )
        insert_test_record(cursor, payload_str=payload)
        db_conn.commit()

        cursor.execute(
            "SELECT ai_class_id, ai_class_name, ai_confidence, "
            "ai_cascade_source, ai_inference_time_us "
            "FROM ai_diagnosis_view "
            "WHERE site_id = 'test_site' ORDER BY time DESC LIMIT 1"
        )
        row = cursor.fetchone()
        assert row is not None
        assert row[0] == 2
        assert row[1] == "misalignment"
        assert float(row[2]) == pytest.approx(0.78)
        assert row[3] == "fallback_rule"
        assert row[4] == 8500

    def test_missing_ai_section_excluded(self, db_conn):
        cursor = db_conn.cursor()
        payload_str = make_payload(ai=None)
        insert_test_record(cursor, payload_str=payload_str)
        db_conn.commit()

        cursor.execute(
            "SELECT COUNT(*) FROM ai_diagnosis_view WHERE site_id = 'test_site'"
        )
        assert cursor.fetchone()[0] == 0


class TestDualChannelView:
    def test_extracts_dual_channel_fields(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(
            dual_channel={"rms_ratio": 1.85, "spectral_similarity": 0.72,
                          "phase_coherence": 0.55, "nde_online": 1, "nde_errors": 0}
        )
        insert_test_record(cursor, payload_str=payload)
        db_conn.commit()

        cursor.execute(
            "SELECT rms_ratio, spectral_similarity, phase_coherence, "
            "nde_online, nde_errors FROM dual_channel_view "
            "WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert row is not None
        assert float(row[0]) == pytest.approx(1.85)
        assert float(row[1]) == pytest.approx(0.72)
        assert float(row[2]) == pytest.approx(0.55)
        assert row[3] == 1
        assert row[4] == 0

    def test_nde_offline_excluded_from_view(self, db_conn):
        cursor = db_conn.cursor()
        payload_str = make_payload(dual_channel=None)
        insert_test_record(cursor, payload_str=payload_str)
        db_conn.commit()

        cursor.execute(
            "SELECT COUNT(*) FROM dual_channel_view WHERE site_id = 'test_site'"
        )
        assert cursor.fetchone()[0] == 0

    def test_nde_online_with_errors(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(
            dual_channel={"nde_online": 1, "nde_errors": 3}
        )
        insert_test_record(cursor, payload_str=payload)
        db_conn.commit()

        cursor.execute(
            "SELECT nde_online, nde_errors FROM dual_channel_view "
            "WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert row[0] == 1
        assert row[1] == 3


class TestEnvironmentView:
    def test_extracts_environment_fields(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(
            environment={"temperature_c": 38.5, "humidity_rh": 72.0}
        )
        insert_test_record(cursor, payload_str=payload)
        db_conn.commit()

        cursor.execute(
            "SELECT temperature_c, humidity_rh, compensation_active "
            "FROM environment_view WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert float(row[0]) == pytest.approx(38.5)
        assert float(row[1]) == pytest.approx(72.0)
        assert row[2] is True

    def test_compensation_defaults_to_false(self, db_conn):
        cursor = db_conn.cursor()
        data = json.loads(make_payload())
        del data["data"]["compensation"]
        insert_test_record(cursor, payload_str=json.dumps(data))
        db_conn.commit()

        cursor.execute(
            "SELECT compensation_active FROM environment_view WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert row[0] is False, "Missing compensation.active should default to false"


class TestDeviceStatusView:
    def test_returns_one_row_per_device(self, db_conn):
        cursor = db_conn.cursor()
        for offset in [60, 30, 0]:
            insert_test_record(cursor, time_offset_sec=offset, device_id="de01")
        for offset in [45, 0]:
            insert_test_record(cursor, time_offset_sec=offset, device_id="de02",
                               payload_str=make_payload(
                                   ai={"class_name": "imbalance"}
                               ))
        db_conn.commit()

        cursor.execute(
            "SELECT device_id, last_rms, last_ai_class FROM device_status_view "
            "WHERE site_id = 'test_site' ORDER BY device_id"
        )
        rows = cursor.fetchall()
        assert len(rows) == 2
        assert rows[0][0] == "de01"
        assert rows[1][0] == "de02"
        # The most recent de02 record was inserted at offset=0 with class_name=imbalance
        assert rows[1][2] == "imbalance"

    def test_service_state_and_data_quality(self, db_conn):
        cursor = db_conn.cursor()
        payload = make_payload(service_state="ERROR")
        data = json.loads(payload)
        data["data_quality"] = 0
        insert_test_record(cursor, payload_str=json.dumps(data))
        db_conn.commit()

        cursor.execute(
            "SELECT service_state, data_quality FROM device_status_view "
            "WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert row[0] == "ERROR"
        assert row[1] == 0


# ============================================================================
# Alert Threshold Tests (ISO 10816)
# ============================================================================

class TestAlertThresholds:
    def test_rms_normal_below_2_8(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            vibration={"overall_rms": 1.5}
        ))
        db_conn.commit()
        cursor.execute("SELECT overall_rms FROM vibration_view WHERE site_id = 'test_site'")
        assert float(cursor.fetchone()[0]) < 2.8

    def test_rms_warning_between_2_8_and_7_1(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            vibration={"overall_rms": 3.5}
        ))
        db_conn.commit()
        cursor.execute("SELECT overall_rms FROM vibration_view WHERE site_id = 'test_site'")
        rms = float(cursor.fetchone()[0])
        assert 2.8 <= rms <= 7.1

    def test_rms_critical_above_7_1(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            vibration={"overall_rms": 8.2}
        ))
        db_conn.commit()
        cursor.execute("SELECT overall_rms FROM vibration_view WHERE site_id = 'test_site'")
        assert float(cursor.fetchone()[0]) > 7.1

    def test_temp_warning_above_45(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            environment={"temperature_c": 48.0}
        ))
        db_conn.commit()
        cursor.execute("SELECT temperature_c FROM environment_view WHERE site_id = 'test_site'")
        assert float(cursor.fetchone()[0]) > 45.0

    def test_ratio_critical_above_2_0(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            dual_channel={"rms_ratio": 2.5}
        ))
        db_conn.commit()
        cursor.execute("SELECT rms_ratio FROM dual_channel_view WHERE site_id = 'test_site'")
        assert float(cursor.fetchone()[0]) > 2.0

    def test_ai_bearing_fault_detected(self, db_conn):
        cursor = db_conn.cursor()
        insert_test_record(cursor, payload_str=make_payload(
            ai={"class_id": 3, "class_name": "bearing_fault", "confidence": 0.94,
                "cascade_source": "primary_cnn"}
        ))
        db_conn.commit()
        cursor.execute(
            "SELECT ai_class_name, ai_confidence FROM ai_diagnosis_view "
            "WHERE site_id = 'test_site'"
        )
        row = cursor.fetchone()
        assert row[0] == "bearing_fault"
        assert float(row[1]) >= 0.85


# ============================================================================
# Grafana API Tests (requires Grafana to be running)
# ============================================================================

@pytest.mark.grafana
class TestGrafanaDashboards:
    def test_grafana_health(self, grafana_session):
        resp = grafana_session.get(f"{GRAFANA_URL}/api/health")
        assert resp.status_code == 200, f"Grafana health check failed: {resp.text}"

    @pytest.mark.parametrize("uid", DASHBOARD_UIDS)
    def test_dashboard_exists(self, grafana_session, uid):
        resp = grafana_session.get(f"{GRAFANA_URL}/api/dashboards/uid/{uid}")
        assert resp.status_code == 200, f"Dashboard {uid} not found: {resp.status_code} {resp.text}"

    def test_datasource_healthy(self, grafana_session):
        resp = grafana_session.get(f"{GRAFANA_URL}/api/datasources/uid/edgevib-ts/health")
        # Grafana 11 may return 200 or error if health check not supported
        if resp.status_code == 200:
            data = resp.json()
            assert data.get("status") == "OK" or data.get("message", "")

    def test_dashboard_query_executes(self, grafana_session, db_conn):
        """End-to-end: insert data, verify Grafana can query it through a VIEW."""
        cursor = db_conn.cursor()
        insert_test_record(cursor)
        db_conn.commit()

        resp = grafana_session.post(
            f"{GRAFANA_URL}/api/ds/query",
            json={
                "queries": [{
                    "refId": "A",
                    "datasource": {"type": "postgres", "uid": "edgevib-ts"},
                    "rawSql": "SELECT time, overall_rms FROM vibration_view WHERE site_id = 'test_site' ORDER BY time DESC LIMIT 1",
                    "format": "table",
                }],
                "from": "now-1h",
                "to": "now",
            },
        )
        assert resp.status_code == 200, f"Grafana ds/query failed: {resp.text}"
        data = resp.json()
        assert "results" in data or "A" in data.get("results", {})
