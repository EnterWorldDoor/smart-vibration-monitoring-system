package filestore

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func newTestLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestSaveAndGetFirmware(t *testing.T) {
	logger := newTestLogger()
	base := t.TempDir()
	store := NewStore(base, logger)

	content := "test firmware content"
	reader := strings.NewReader(content)

	filePath, fileSize, sha256Hash, err := store.SaveFirmware(reader, "esp32", "test-v1.0.0.bin")
	if err != nil {
		t.Fatalf("SaveFirmware failed: %v", err)
	}

	expectedLen := int64(len(content))
	if fileSize != expectedLen {
		t.Errorf("expected size %d, got %d", expectedLen, fileSize)
	}

	expectedPath := filepath.Join("esp32", "test-v1.0.0.bin")
	if filePath != expectedPath {
		t.Errorf("expected path %s, got %s", expectedPath, filePath)
	}

	hasher := sha256.New()
	hasher.Write([]byte(content))
	expectedSHA256 := hex.EncodeToString(hasher.Sum(nil))
	if sha256Hash != expectedSHA256 {
		t.Errorf("SHA256 mismatch")
	}

	fullPath, err := store.GetFirmwarePath("esp32", "test-v1.0.0.bin")
	if err != nil {
		t.Fatalf("GetFirmwarePath failed: %v", err)
	}
	data, err := os.ReadFile(fullPath)
	if err != nil {
		t.Fatalf("ReadFile failed: %v", err)
	}
	if string(data) != content {
		t.Errorf("file content mismatch")
	}
}

func TestGetFirmwarePathTraversal(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	_, err := store.GetFirmwarePath("esp32", "../../etc/passwd")
	if err == nil {
		t.Fatal("expected error for path traversal")
	}
}

func TestGetFirmwarePathInvalidExtension(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	_, err := store.GetFirmwarePath("esp32", "malware.exe")
	if err == nil {
		t.Fatal("expected error for invalid extension")
	}
}

func TestDeleteFirmware(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	_, _, _, err := store.SaveFirmware(strings.NewReader("test"), "esp32", "delete-test.bin")
	if err != nil {
		t.Fatalf("SaveFirmware failed: %v", err)
	}

	if err := store.DeleteFirmware("esp32", "delete-test.bin"); err != nil {
		t.Fatalf("DeleteFirmware failed: %v", err)
	}

	_, err = store.GetFirmwarePath("esp32", "delete-test.bin")
	if err != nil {
		t.Fatalf("GetFirmwarePath should still return a path even if file is deleted: %v", err)
	}
}

func TestDeleteFirmwareNonexistent(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	if err := store.DeleteFirmware("esp32", "nonexistent.bin"); err != nil {
		t.Fatalf("DeleteFirmware should not error on nonexistent file: %v", err)
	}
}

func TestListFirmware(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	store.SaveFirmware(strings.NewReader("a"), "esp32", "a.bin")
	store.SaveFirmware(strings.NewReader("b"), "esp32", "b.bin")

	files, err := store.ListFirmware("esp32")
	if err != nil {
		t.Fatalf("ListFirmware failed: %v", err)
	}
	if len(files) != 2 {
		t.Errorf("expected 2 files, got %d", len(files))
	}
}

func TestListFirmwareEmpty(t *testing.T) {
	logger := newTestLogger()
	store := NewStore(t.TempDir(), logger)

	files, err := store.ListFirmware("nonexistent")
	if err != nil {
		t.Fatalf("ListFirmware failed: %v", err)
	}
	if files != nil {
		t.Errorf("expected nil for nonexistent dir, got %v", files)
	}
}
