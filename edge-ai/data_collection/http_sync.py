#!/usr/bin/env python3
"""
EdgeVib HTTP Data Syncer — Production-mode training data pull from api-server.

Replaces MQTT direct subscription (development mode) with HTTP incremental
pull from Orange Pi api-server (production mode).

Usage:
    python data_collection/http_sync.py --api http://192.168.1.1:8080
    python data_collection/http_sync.py --api http://192.168.1.1:8080 --from 2026-05-01T00:00:00Z
"""

import argparse
import os
import sys
import time
from datetime import datetime, timezone

import requests


DEFAULT_API_URL = "http://192.168.1.1:8080"
DEFAULT_OUTPUT = "training_data.csv"
DEFAULT_CURSOR_FILE = "last_sync.txt"
DEFAULT_TIMEOUT = 30
FIRST_SYNC_DEFAULT = "1970-01-01T00:00:00Z"


class SyncError(Exception):
    """Raised when sync fails and should be retried later."""


class HttpDataSyncer:
    """Pulls training data from api-server export endpoint with incremental cursor."""

    def __init__(
        self,
        api_url=DEFAULT_API_URL,
        output_path=DEFAULT_OUTPUT,
        cursor_file=DEFAULT_CURSOR_FILE,
        from_override=None,
        to_override=None,
        sites=None,
        devices=None,
        timeout=DEFAULT_TIMEOUT,
    ):
        self.api_url = api_url.rstrip("/")
        self.output_path = os.path.abspath(output_path)
        self.cursor_file = os.path.join(
            os.path.dirname(self.output_path) or ".",
            cursor_file,
        )
        self.from_override = from_override
        self.to_override = to_override
        self.sites = sites
        self.devices = devices
        self.timeout = timeout

    # ------------------------------------------------------------------
    # Cursor management
    # ------------------------------------------------------------------

    def _read_cursor(self):
        if os.path.exists(self.cursor_file):
            with open(self.cursor_file, "r") as f:
                val = f.read().strip()
                if val:
                    return val
        return None

    def _save_cursor(self, timestamp_iso):
        tmp = self.cursor_file + ".tmp"
        with open(tmp, "w") as f:
            f.write(timestamp_iso)
        os.replace(tmp, self.cursor_file)

    def _clear_cursor(self):
        if os.path.exists(self.cursor_file):
            os.remove(self.cursor_file)

    # ------------------------------------------------------------------
    # Sync logic
    # ------------------------------------------------------------------

    def sync(self):
        """Execute one sync cycle. Returns number of lines written (excluding header)."""
        # Determine time range
        if self.from_override:
            from_iso = self.from_override
        else:
            from_iso = self._read_cursor()
            if from_iso is None:
                from_iso = FIRST_SYNC_DEFAULT
                print(f"[sync] No cursor found, using first-sync default: {from_iso}")

        to_iso = self.to_override or datetime.now(timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"
        )

        # Build URL
        params = {"from": from_iso, "to": to_iso, "format": "csv"}
        if self.sites:
            params["sites"] = ",".join(self.sites)
        if self.devices:
            params["devices"] = ",".join(self.devices)

        url = f"{self.api_url}/api/v1/data/export"
        print(f"[sync] GET {url}")
        print(f"[sync] Range: {from_iso} -> {to_iso}")

        # HTTP GET with stream for potentially large responses
        try:
            resp = requests.get(url, params=params, timeout=self.timeout, stream=True)
        except requests.exceptions.RequestException as e:
            raise SyncError(f"HTTP request failed: {e}")

        if resp.status_code != 200:
            body = ""
            try:
                body = resp.text[:500]
            except Exception:
                pass
            raise SyncError(
                f"Server returned {resp.status_code}: {body}"
            )

        # Write to CSV
        file_exists = (
            os.path.exists(self.output_path)
            and os.path.getsize(self.output_path) > 0
        )
        lines_written = 0

        try:
            with open(self.output_path, "a", newline="", encoding="utf-8") as f:
                first_line = True
                for line in resp.iter_lines(decode_unicode=True):
                    if line is None:
                        continue
                    if first_line and file_exists:
                        # Skip CSV header when appending to existing file
                        first_line = False
                        continue
                    first_line = False
                    f.write(line + "\n")
                    lines_written += 1
        except IOError as e:
            raise SyncError(f"CSV write failed: {e}")

        # Update cursor (only after successful CSV write)
        if lines_written > 0:
            self._save_cursor(to_iso)
            print(f"[sync] Wrote {lines_written} lines, cursor updated to {to_iso}")
        else:
            print(f"[sync] No new data in range, cursor unchanged")

        return lines_written


# ------------------------------------------------------------------
# CLI
# ------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(
        description="EdgeVib Training Data HTTP Syncer — pull from api-server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --api http://192.168.1.1:8080
  %(prog)s --api http://192.168.1.1:8080 --from 2026-05-01T00:00:00Z --sites factory1
  %(prog)s --api http://192.168.1.1:8080 --devices de01,nde01
        """,
    )
    parser.add_argument(
        "--api-url",
        default=DEFAULT_API_URL,
        help=f"api-server base URL (default: {DEFAULT_API_URL})",
    )
    parser.add_argument(
        "--output",
        default=DEFAULT_OUTPUT,
        help=f"Output CSV file path (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--cursor-file",
        default=DEFAULT_CURSOR_FILE,
        help=f"Cursor timestamp file (default: {DEFAULT_CURSOR_FILE})",
    )
    parser.add_argument(
        "--from",
        dest="from_",
        default=None,
        help="Override start time (RFC3339). Default: auto from cursor.",
    )
    parser.add_argument(
        "--to",
        default=None,
        help="Override end time (RFC3339). Default: current UTC time.",
    )
    parser.add_argument(
        "--sites",
        default=None,
        help="Comma-separated site IDs to filter (default: all)",
    )
    parser.add_argument(
        "--devices",
        default=None,
        help="Comma-separated device IDs to filter (default: all)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT,
        help=f"HTTP timeout in seconds (default: {DEFAULT_TIMEOUT})",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    sites = [s.strip() for s in args.sites.split(",") if s.strip()] if args.sites else None
    devices = [d.strip() for d in args.devices.split(",") if d.strip()] if args.devices else None

    syncer = HttpDataSyncer(
        api_url=args.api_url,
        output_path=args.output,
        cursor_file=args.cursor_file,
        from_override=args.from_,
        to_override=args.to,
        sites=sites,
        devices=devices,
        timeout=args.timeout,
    )

    try:
        lines = syncer.sync()
        print(f"Done: {lines} lines synced")
    except SyncError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
