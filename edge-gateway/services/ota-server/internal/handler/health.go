package handler

import (
	"context"
	"net/http"

	"github.com/go-chi/chi/v5"
)

type HealthChecker interface {
	PingHealth(ctx context.Context) error
}

type HealthHandler struct {
	db           HealthChecker
	mqttConnected func() bool
}

func NewHealthHandler(db HealthChecker, mqttConnected func() bool) *HealthHandler {
	return &HealthHandler{db: db, mqttConnected: mqttConnected}
}

func (h *HealthHandler) Register(r chi.Router) {
	r.Get("/api/v1/health", h.ServeHealth)
}

func (h *HealthHandler) ServeHealth(w http.ResponseWriter, r *http.Request) {
	dbStatus := "ok"
	if err := h.db.PingHealth(r.Context()); err != nil {
		dbStatus = "error"
	}
	mqttStatus := "ok"
	if !h.mqttConnected() {
		mqttStatus = "error"
	}
	writeJSON(w, http.StatusOK, map[string]string{
		"db":   dbStatus,
		"mqtt": mqttStatus,
	})
}
