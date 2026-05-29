"""Tests for http_sync.py — mock HTTP, verify cursor logic and CSV writing."""

import os
import sys
import tempfile
from unittest.mock import patch, MagicMock
import pytest

# Add edge-ai to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from data_collection.http_sync import HttpDataSyncer, SyncError


CSV_HEADER = "time,site_id,device_type,device_id,rms_x,rms_y,rms_z,overall_rms,peak_freq,peak_amp,temperature_c,humidity_rh\n"
CSV_ROW1 = "2026-05-01T00:00:00Z,factory1,motor,de01,0.1,0.2,0.3,0.4,120.0,0.05,25.0,50.0\n"
CSV_ROW2 = "2026-05-01T00:00:02Z,factory1,motor,de01,0.11,0.21,0.31,0.41,121.0,0.06,25.1,50.1\n"


class MockResponse:
    def __init__(self, status_code, lines=None, text="", iter_content=None):
        self.status_code = status_code
        self._lines = lines or []
        self.text = text

    def iter_lines(self, decode_unicode=True):
        yield from self._lines

    def iter_content(self, chunk_size=None):
        yield from (self._lines or [])


@pytest.fixture
def tmpdir():
    with tempfile.TemporaryDirectory() as td:
        yield td


def test_read_cursor_first_time(tmpdir):
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=os.path.join(tmpdir, "cursor.txt"), output_path=os.path.join(tmpdir, "out.csv"))
    assert syncer._read_cursor() is None


def test_read_cursor_existing(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    with open(cursor_file, "w") as f:
        f.write("2026-05-01T00:00:00Z")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=os.path.join(tmpdir, "out.csv"))
    assert syncer._read_cursor() == "2026-05-01T00:00:00Z"


def test_save_cursor_atomic(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=os.path.join(tmpdir, "out.csv"))
    syncer._save_cursor("2026-05-01T12:00:00Z")
    assert syncer._read_cursor() == "2026-05-01T12:00:00Z"
    assert not os.path.exists(cursor_file + ".tmp")


def test_sync_success_first_time(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=output_path)

    mock_resp = MockResponse(200, lines=[CSV_HEADER.strip(), CSV_ROW1.strip(), CSV_ROW2.strip()])
    with patch("data_collection.http_sync.requests.get", return_value=mock_resp):
        lines = syncer.sync()

    assert lines == 2  # header skipped, 2 data rows
    assert os.path.exists(cursor_file)
    with open(output_path) as f:
        content = f.read()
        assert "time,site_id" in content
        assert "de01" in content


def test_sync_append_skips_header(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    output_path = os.path.join(tmpdir, "out.csv")
    # Pre-populate file
    with open(output_path, "w") as f:
        f.write(CSV_HEADER)
        f.write(CSV_ROW1)

    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=output_path)
    mock_resp = MockResponse(200, lines=[CSV_HEADER.strip(), CSV_ROW2.strip()])
    with patch("data_collection.http_sync.requests.get", return_value=mock_resp):
        lines = syncer.sync()

    assert lines == 1  # only the new data row (header skipped)
    with open(output_path) as f:
        lines_read = f.readlines()
        assert len(lines_read) == 3  # header + original row + new row


def test_sync_http_error_preserves_cursor(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    with open(cursor_file, "w") as f:
        f.write("2026-05-01T00:00:00Z")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=output_path)

    mock_resp = MockResponse(500, text="Internal Server Error")
    with patch("data_collection.http_sync.requests.get", return_value=mock_resp):
        with pytest.raises(SyncError):
            syncer.sync()

    # Cursor must not change
    assert syncer._read_cursor() == "2026-05-01T00:00:00Z"


def test_sync_network_error_preserves_cursor(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    with open(cursor_file, "w") as f:
        f.write("2026-05-01T00:00:00Z")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=output_path)

    with patch("data_collection.http_sync.requests.get", side_effect=Exception("Connection refused")):
        with pytest.raises(SyncError):
            syncer.sync()

    assert syncer._read_cursor() == "2026-05-01T00:00:00Z"


def test_sync_empty_response_no_cursor_update(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    with open(cursor_file, "w") as f:
        f.write("2026-05-01T00:00:00Z")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=output_path)

    # Header only, no data rows
    mock_resp = MockResponse(200, lines=[CSV_HEADER.strip()])
    with patch("data_collection.http_sync.requests.get", return_value=mock_resp):
        lines = syncer.sync()

    assert lines == 0
    assert syncer._read_cursor() == "2026-05-01T00:00:00Z"  # unchanged


def test_sync_with_site_filter(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(
        api_url="http://test:8080",
        cursor_file=cursor_file,
        output_path=output_path,
        sites=["factory1"],
    )

    mock_resp = MockResponse(200, lines=[CSV_HEADER.strip()])
    with patch("data_collection.http_sync.requests.get") as mock_get:
        mock_get.return_value = mock_resp
        syncer.sync()
        # Verify sites param in URL
        call_args = mock_get.call_args
        assert "sites=factory1" in str(call_args)


def test_sync_with_from_override(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    output_path = os.path.join(tmpdir, "out.csv")
    syncer = HttpDataSyncer(
        api_url="http://test:8080",
        cursor_file=cursor_file,
        output_path=output_path,
        from_override="2026-01-01T00:00:00Z",
    )

    mock_resp = MockResponse(200, lines=[CSV_HEADER.strip()])
    with patch("data_collection.http_sync.requests.get") as mock_get:
        mock_get.return_value = mock_resp
        syncer.sync()
        call_args = mock_get.call_args
        assert "2026-01-01T00:00:00Z" in str(call_args)


def test_clear_cursor(tmpdir):
    cursor_file = os.path.join(tmpdir, "cursor.txt")
    with open(cursor_file, "w") as f:
        f.write("2026-05-01T00:00:00Z")
    syncer = HttpDataSyncer(api_url="http://test:8080", cursor_file=cursor_file, output_path=os.path.join(tmpdir, "out.csv"))
    syncer._clear_cursor()
    assert not os.path.exists(cursor_file)
