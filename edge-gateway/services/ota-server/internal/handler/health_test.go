package handler

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/go-chi/chi/v5"
)

type mockHealthChecker struct {
	err error
}

func (m *mockHealthChecker) PingHealth(ctx context.Context) error {
	return m.err
}

func TestHealthOK(t *testing.T) {
	mqttOK := func() bool { return true }
	h := NewHealthHandler(&mockHealthChecker{}, mqttOK)

	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}

	var resp map[string]string
	json.Unmarshal(rec.Body.Bytes(), &resp)

	if resp["db"] != "ok" {
		t.Errorf("expected db=ok, got %s", resp["db"])
	}
	if resp["mqtt"] != "ok" {
		t.Errorf("expected mqtt=ok, got %s", resp["mqtt"])
	}
}

func TestHealthDBError(t *testing.T) {
	mqttOK := func() bool { return true }
	h := NewHealthHandler(&mockHealthChecker{err: context.DeadlineExceeded}, mqttOK)

	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}

	var resp map[string]string
	json.Unmarshal(rec.Body.Bytes(), &resp)

	if resp["db"] != "error" {
		t.Errorf("expected db=error, got %s", resp["db"])
	}
}

func TestHealthMQTTDown(t *testing.T) {
	mqttDown := func() bool { return false }
	h := NewHealthHandler(&mockHealthChecker{}, mqttDown)

	r := chi.NewRouter()
	h.Register(r)

	req := httptest.NewRequest("GET", "/api/v1/health", nil)
	rec := httptest.NewRecorder()
	r.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rec.Code)
	}

	var resp map[string]string
	json.Unmarshal(rec.Body.Bytes(), &resp)

	if resp["mqtt"] != "error" {
		t.Errorf("expected mqtt=error when mqtt is down, got %s", resp["mqtt"])
	}
}
