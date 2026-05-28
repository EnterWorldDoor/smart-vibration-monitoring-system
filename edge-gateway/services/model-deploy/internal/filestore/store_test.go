package filestore

import (
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestSaveAndGetModel(t *testing.T) {
	dir := t.TempDir()
	store := NewStore(dir, slog.Default())

	content := "mock onnx model content"
	relPath, size, sha256, err := store.SaveModel(strings.NewReader(content), "autoencoder", "1.0.0")
	if err != nil {
		t.Fatalf("SaveModel failed: %v", err)
	}
	if relPath != filepath.Join("autoencoder", "1.0.0.onnx") {
		t.Errorf("unexpected relPath: %s", relPath)
	}
	if size != int64(len(content)) {
		t.Errorf("expected size %d, got %d", len(content), size)
	}
	if sha256 == "" {
		t.Error("sha256 should not be empty")
	}

	fullPath, err := store.GetModelPath("autoencoder", "1.0.0")
	if err != nil {
		t.Fatalf("GetModelPath failed: %v", err)
	}
	if !filepath.IsAbs(fullPath) {
		t.Errorf("expected absolute path, got %s", fullPath)
	}
	data, err := os.ReadFile(fullPath)
	if err != nil {
		t.Fatalf("read saved file: %v", err)
	}
	if string(data) != content {
		t.Error("file content mismatch")
	}
}

func TestGetModelPathTraversal(t *testing.T) {
	store := NewStore(t.TempDir(), slog.Default())
	_, err := store.GetModelPath("../etc", "1.0.0")
	if err == nil {
		t.Error("expected error for path traversal in model_name")
	}
	_, err = store.GetModelPath("autoencoder", "../1.0.0")
	if err == nil {
		t.Error("expected error for path traversal in version")
	}
}

func TestGetModelPathEmpty(t *testing.T) {
	store := NewStore(t.TempDir(), slog.Default())
	_, err := store.GetModelPath("", "1.0.0")
	if err == nil {
		t.Error("expected error for empty model_name")
	}
	_, err = store.GetModelPath("autoencoder", "")
	if err == nil {
		t.Error("expected error for empty version")
	}
}

func TestDeleteModel(t *testing.T) {
	dir := t.TempDir()
	store := NewStore(dir, slog.Default())

	content := "delete me"
	_, _, _, err := store.SaveModel(strings.NewReader(content), "autoencoder", "1.0.0")
	if err != nil {
		t.Fatalf("SaveModel: %v", err)
	}

	if err := store.DeleteModel("autoencoder", "1.0.0"); err != nil {
		t.Fatalf("DeleteModel: %v", err)
	}

	_, err = store.GetModelPath("autoencoder", "1.0.0")
	if err != nil {
		t.Fatalf("GetModelPath: %v", err)
	}
	// file should be gone
}

func TestDeleteModelNonexistent(t *testing.T) {
	store := NewStore(t.TempDir(), slog.Default())
	err := store.DeleteModel("autoencoder", "99.99.99")
	if err != nil {
		t.Errorf("DeleteModel should not error on nonexistent file: %v", err)
	}
}

func TestRotateVersions(t *testing.T) {
	dir := t.TempDir()
	store := NewStore(dir, slog.Default())

	versions := []string{"1.0.0", "1.1.0", "1.2.0", "1.3.0", "1.4.0"}
	for _, v := range versions {
		_, _, _, err := store.SaveModel(strings.NewReader("v"+v), "autoencoder", v)
		if err != nil {
			t.Fatalf("SaveModel %s: %v", v, err)
		}
	}

	if err := store.RotateVersions("autoencoder", 3); err != nil {
		t.Fatalf("RotateVersions: %v", err)
	}

	entries, _ := os.ReadDir(filepath.Join(dir, "autoencoder"))
	count := 0
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".onnx") {
			count++
		}
	}
	if count != 3 {
		t.Errorf("expected 3 files after rotation, got %d", count)
	}
}

func TestRotateBelowKeep(t *testing.T) {
	dir := t.TempDir()
	store := NewStore(dir, slog.Default())

	store.SaveModel(strings.NewReader("v1"), "autoencoder", "1.0.0")
	store.SaveModel(strings.NewReader("v2"), "autoencoder", "1.1.0")

	if err := store.RotateVersions("autoencoder", 5); err != nil {
		t.Fatalf("RotateVersions: %v", err)
	}

	entries, _ := os.ReadDir(filepath.Join(dir, "autoencoder"))
	count := 0
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".onnx") {
			count++
		}
	}
	if count != 2 {
		t.Errorf("expected 2 files (keep > count), got %d", count)
	}
}
