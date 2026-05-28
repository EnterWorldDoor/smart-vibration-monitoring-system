package handler

import (
	"net/http"

	"edgevib/ota-server/internal/db"

	"github.com/go-chi/chi/v5"
)

type HealthHandler struct {
	db           *db.Client
	mqttConnected func() bool
}

func NewHealthHandler(dbClient *db.Client, mqttConnected func() bool) *HealthHandler {
	return &HealthHandler{db: dbClient, mqttConnected: mqttConnected}
}

func (h *HealthHandler) Register(r chi.Router) {
	r.Get("/api/v1/health", h.ServeHealth)
}

func (h *HealthHandler) ServeHealth(w http.ResponseWriter, r *http.Request) {
	dbStatus := "ok"
	if err := h.db.Ping(r.Context()); err != nil {
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
