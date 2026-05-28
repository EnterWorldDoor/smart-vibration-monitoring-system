package handler

import (
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"

	"edgevib/ota-server/internal/filestore"

	"github.com/go-chi/chi/v5"
)

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestServeFirmwareFile(t *testing.T) {
	store := filestore.NewStore(t.TempDir(), testLogger())

	_, _, _, err := store.SaveFirmware(
		&fakeReader{data: "fake firmware"}, "esp32", "test-v1.0.0.bin",
	)
	if err != nil {
		t.Fatalf("SaveFirmware failed: %v", err)
	}

	h := &VersionHandler{db: nil, store: store}
	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/firmware/esp32/test-v1.0.0.bin", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}
	if rec.Body.String() != "fake firmware" {
		t.Errorf("expected 'fake firmware', got '%s'", rec.Body.String())
	}
}

func TestServeFirmwareFileInvalidPlatform(t *testing.T) {
	store := filestore.NewStore(t.TempDir(), testLogger())
	h := &VersionHandler{db: nil, store: store}
	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/firmware/invalid/test.bin", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400 for invalid platform, got %d", rec.Code)
	}
}

func TestServeFirmwareFileNotFound(t *testing.T) {
	store := filestore.NewStore(t.TempDir(), testLogger())
	h := &VersionHandler{db: nil, store: store}
	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/firmware/esp32/nonexistent.bin", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusNotFound {
		t.Errorf("expected 404, got %d", rec.Code)
	}
}

func TestServeFirmwareFilePathTraversal(t *testing.T) {
	store := filestore.NewStore(t.TempDir(), testLogger())
	h := &VersionHandler{db: nil, store: store}
	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/firmware/esp32/../../../etc/passwd", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400 for path traversal, got %d", rec.Code)
	}
}

func TestWriteJSON(t *testing.T) {
	rec := httptest.NewRecorder()
	writeJSON(rec, http.StatusOK, map[string]string{"key": "value"})

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}
	if ct := rec.Header().Get("Content-Type"); ct != "application/json" {
		t.Errorf("expected application/json, got %s", ct)
	}

	var resp map[string]string
	json.Unmarshal(rec.Body.Bytes(), &resp)
	if resp["key"] != "value" {
		t.Errorf("expected key=value, got %v", resp)
	}
}

func TestWriteError(t *testing.T) {
	rec := httptest.NewRecorder()
	writeError(rec, http.StatusBadRequest, "test error")

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rec.Code)
	}

	var resp map[string]string
	json.Unmarshal(rec.Body.Bytes(), &resp)
	if resp["error"] != "test error" {
		t.Errorf("expected error='test error', got %v", resp)
	}
}

type fakeReader struct {
	data string
	pos  int
}

func (r *fakeReader) Read(p []byte) (n int, err error) {
	if r.pos >= len(r.data) {
		return 0, io.EOF
	}
	n = copy(p, r.data[r.pos:])
	r.pos += n
	return n, nil
}
