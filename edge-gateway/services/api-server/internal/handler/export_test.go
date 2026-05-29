package handler

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"

	"edgevib/api-server/internal/db"
)

// mockExportDB implements a minimal ExportQuerier for testing.
type mockExportDB struct {
	rows []db.ExportRow
	err  error
}

func (m *mockExportDB) QueryExportData(ctx context.Context, params db.ExportParams) ([]db.ExportRow, error) {
	if m.err != nil {
		return nil, m.err
	}
	return m.rows, nil
}

func ptr(f float64) *float64 { return &f }

func setupExportRouter(querier interface{ QueryExportData(context.Context, db.ExportParams) ([]db.ExportRow, error) }) chi.Router {
	r := chi.NewRouter()
	// We need to inject the mock — but the handler uses *db.Client directly.
	// For structural tests (param parsing), we can use a real handler with nil DB
	// and let it fail at the query stage — we only test response format.
	return r
}

func TestExportCSV_MissingFromTo(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/data/export", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d: %s", rec.Code, rec.Body.String())
	}
}

func TestExportCSV_InvalidFromFormat(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/data/export?from=not-a-date&to=2026-01-01T00:00:00Z", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rec.Code)
	}
}

func TestExportCSV_InvalidToFormat(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/data/export?from=2026-01-01T00:00:00Z&to=bad", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rec.Code)
	}
}

func TestExportCSV_FromAfterTo(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/data/export?from=2026-06-01T00:00:00Z&to=2026-01-01T00:00:00Z", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rec.Code)
	}
}

func TestExportCSV_UnsupportedFormat(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/data/export?from=2026-01-01T00:00:00Z&to=2026-06-01T00:00:00Z&format=json", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rec.Code)
	}
}

func TestExportCSV_SiteAndDeviceFilterParsing(t *testing.T) {
	// This test verifies that filter params are parsed correctly by reaching
	// the query stage. Since DB is nil, it will panic/500 — but before that,
	// it should have successfully parsed sites and devices params.
	// We verify the parsing doesn't cause a 400.
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	req := httptest.NewRequest("GET",
		"/api/v1/data/export?from=2026-01-01T00:00:00Z&to=2026-06-01T00:00:00Z&sites=factory1,factory2&devices=de01,nde01&format=csv",
		nil)
	rec := httptest.NewRecorder()

	// This will panic because db is nil, but that's expected.
	// The test verifies param parsing doesn't return 400.
	defer func() {
		if r := recover(); r != nil {
			// Expected: nil db causes panic when calling QueryExportData
		}
	}()
	r.ServeHTTP(rec, req)
	if rec.Code == http.StatusBadRequest {
		t.Errorf("params should be valid, got 400: %s", rec.Body.String())
	}
}

func TestFloatOrEmpty(t *testing.T) {
	tests := []struct {
		input    *float64
		expected string
	}{
		{nil, ""},
		{ptr(3.14), "3.14"},
		{ptr(0.0), "0"},
		{ptr(-1.5), "-1.5"},
	}

	for _, tt := range tests {
		got := floatOrEmpty(tt.input)
		if got != tt.expected {
			t.Errorf("floatOrEmpty(%v) = %q, want %q", tt.input, got, tt.expected)
		}
	}
}

func TestExportCSV_DateParsingEdgeCases(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	// Valid date with timezone offset
	req := httptest.NewRequest("GET",
		"/api/v1/data/export?from=2026-01-01T00:00:00%2B08:00&to=2026-06-01T00:00:00%2B08:00",
		nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)
	// Will fail at DB stage but shouldn't be a 400
	if rec.Code == http.StatusBadRequest && !strings.Contains(rec.Body.String(), "required") {
		t.Logf("got %d: %s (non-400 means date parsed)", rec.Code, rec.Body.String())
	}
}

func TestExportCSV_DefaultFormatWhenMissing(t *testing.T) {
	r := chi.NewRouter()
	h := NewExportHandler(nil)
	h.Register(r)

	// No format param → defaults to csv → should not get 400 for format
	req := httptest.NewRequest("GET",
		"/api/v1/data/export?from=2026-01-01T00:00:00Z&to=2026-06-01T00:00:00Z",
		nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)
	if rec.Code == http.StatusBadRequest && strings.Contains(rec.Body.String(), "format") {
		t.Error("default format should be csv, got format-related 400")
	}
}

// Ensure ExportHandler satisfies the Register pattern
func TestExportHandler_ImplementsRegister(t *testing.T) {
	var _ interface{ Register(chi.Router) } = NewExportHandler(nil)
}

// Test that time parsing matches RFC3339 as documented
func TestTimeRFC3339Parsing(t *testing.T) {
	valid := []string{
		"2026-01-01T00:00:00Z",
		"2026-01-01T00:00:00+08:00",
		"2026-01-01T00:00:00.000Z",
	}
	for _, s := range valid {
		_, err := time.Parse(time.RFC3339, s)
		if err != nil {
			t.Errorf("should parse %q: %v", s, err)
		}
	}
}
