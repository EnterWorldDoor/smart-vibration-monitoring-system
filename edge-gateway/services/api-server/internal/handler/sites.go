package handler

import (
	"encoding/json"
	"net/http"

	"edgevib/api-server/internal/db"

	"github.com/go-chi/chi/v5"
)

type SitesHandler struct {
	db *db.Client
}

func NewSitesHandler(dbClient *db.Client) *SitesHandler {
	return &SitesHandler{db: dbClient}
}

func (h *SitesHandler) Register(r chi.Router) {
	r.Get("/api/v1/sites", h.ListSites)
	r.Get("/api/v1/sites/{site_id}/overview", h.GetOverview)
	r.Get("/api/v1/sites/{site_id}/devices", h.ListDevices)
}

func (h *SitesHandler) ListSites(w http.ResponseWriter, r *http.Request) {
	sites, err := h.db.ListSites(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, sites)
}

func (h *SitesHandler) GetOverview(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	devices, err := h.db.GetSiteOverview(r.Context(), siteID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id": siteID,
		"devices": devices,
	})
}

func (h *SitesHandler) ListDevices(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	devices, err := h.db.ListDevices(r.Context(), siteID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id": siteID,
		"devices": devices,
	})
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}
