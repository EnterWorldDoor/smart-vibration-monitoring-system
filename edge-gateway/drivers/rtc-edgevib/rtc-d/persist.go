/*
 * persist.go — Time file persistence
 *
 * Reads/writes Unix epoch seconds to a flat file.
 * Format: ASCII integer (10 digits) + trailing newline.
 * Compatible with: date -s @$(cat /var/lib/edgevib/last_time)
 *
 * Reference: flush-d/buffer_io.go for file I/O patterns
 */

package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

const persistFileMode = 0644

/* ---- TimeFile ---- */

type TimeFile struct {
	path string
}

// NewTimeFile creates a TimeFile handler.
func NewTimeFile(path string) *TimeFile {
	return &TimeFile{path: path}
}

// Path returns the file path.
func (t *TimeFile) Path() string {
	return t.path
}

// Read reads Unix epoch from file.
// Returns zero time if file does not exist or is unparseable.
func (t *TimeFile) Read() (time.Time, error) {
	data, err := os.ReadFile(t.path)
	if err != nil {
		if os.IsNotExist(err) {
			return time.Time{}, fmt.Errorf("time file not found: %s", t.path)
		}
		return time.Time{}, fmt.Errorf("read %s: %w", t.path, err)
	}

	s := strings.TrimSpace(string(data))
	sec, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return time.Time{}, fmt.Errorf("parse epoch from %s: %w", t.path, err)
	}

	if sec <= 0 {
		return time.Time{}, fmt.Errorf("invalid epoch in %s: %d", t.path, sec)
	}

	return time.Unix(sec, 0).UTC(), nil
}

// Write writes Unix epoch to file.
func (t *TimeFile) Write(tm time.Time) error {
	epoch := tm.Unix()
	payload := fmt.Sprintf("%d\n", epoch)

	// Atomic-ish write: create temp file, then rename
	tmpPath := t.path + ".tmp"
	if err := os.WriteFile(tmpPath, []byte(payload), persistFileMode); err != nil {
		return fmt.Errorf("write %s: %w", tmpPath, err)
	}
	if err := os.Rename(tmpPath, t.path); err != nil {
		return fmt.Errorf("rename %s → %s: %w", tmpPath, t.path, err)
	}

	return nil
}
