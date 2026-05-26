"""Test DB client SQL correctness using mock psycopg2."""

from unittest.mock import MagicMock, patch, call
from datetime import datetime, timezone

import pytest

from src.config import DBConfig
from src.db.client import DBClient


@pytest.fixture
def db_client():
    cfg = DBConfig()
    client = DBClient(cfg)
    client._conn = MagicMock()
    return client


class TestInsertLLMReport:
    def test_insert_params_bound_correctly(self, db_client):
        now = datetime(2026, 5, 26, 10, 0, 0, tzinfo=timezone.utc)

        db_client.insert_llm_report(
            time=now,
            site_id="factory1",
            device_id="de01",
            report_type="alert_report",
            severity="CRITICAL",
            title="Test Title",
            summary="Test summary",
            analysis="Test analysis",
            advice="Test advice",
            raw_output="raw text",
            model_name="qwen2.5-1.5b-instruct",
            model_version="q4_k_m",
            tokens_used=245,
            generation_time_ms=15234.5,
            trigger_reason="ai_bearing_fault",
        )

        db_client._conn.cursor.return_value.__enter__.return_value.execute.assert_called_once()
        call_args = db_client._conn.cursor.return_value.__enter__.return_value.execute.call_args
        sql, params = call_args[0]

        assert "INSERT INTO llm_reports" in sql
        assert params[0] == now
        assert params[1] == "factory1"
        assert params[2] == "de01"
        assert params[3] == "alert_report"
        assert params[4] == "CRITICAL"
        assert params[5] == "Test Title"
        assert params[9] == "raw text"
        assert params[10] == "qwen2.5-1.5b-instruct"
        assert params[12] == 245
        assert params[13] == 15234.5
        assert params[14] == "ai_bearing_fault"
        db_client._conn.commit.assert_called_once()


class TestQueryAIReports:
    def test_query_recent_ai_reports(self, db_client):
        db_client.query_recent_ai_reports("factory1", "de01", 10, 5)

        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.execute.assert_called_once()
        sql, params = cursor.execute.call_args[0]
        assert "ai_reports" in sql
        assert params == ("factory1", "de01", 10, 5)

    def test_query_ai_reports_returns_empty(self, db_client):
        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.fetchall.return_value = []

        result = db_client.query_recent_ai_reports("factory1", "de01")
        assert result == []


class TestQueryVibration:
    def test_query_recent_vibration(self, db_client):
        db_client.query_recent_vibration("factory1", "de01", 10, 30)

        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.execute.assert_called_once()
        sql, params = cursor.execute.call_args[0]
        assert "vibration_view" in sql
        assert params == ("factory1", "de01", 10, 30)

    def test_query_vibration_returns_empty(self, db_client):
        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.fetchall.return_value = []

        result = db_client.query_recent_vibration("factory1", "de01")
        assert result == []


class TestQueryAIReportsInWindow:
    def test_query_window(self, db_client):
        db_client.query_ai_reports_in_window("factory1", 8)

        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.execute.assert_called_once()
        sql, params = cursor.execute.call_args[0]
        assert "ai_reports" in sql
        assert params == ("factory1", 8)


class TestPing:
    def test_ping_success(self, db_client):
        assert db_client.ping()

    def test_ping_failure(self, db_client):
        cursor = db_client._conn.cursor.return_value.__enter__.return_value
        cursor.execute.side_effect = Exception("connection lost")
        assert not db_client.ping()
