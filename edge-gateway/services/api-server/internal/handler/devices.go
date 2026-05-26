package handler

import (
	"net/http"
	"strconv"
	"time"

	"edgevib/api-server/internal/db"

	"github.com/go-chi/chi/v5"
)

type DevicesHandler struct {
	db *db.Client
}

func NewDevicesHandler(dbClient *db.Client) *DevicesHandler {
	return &DevicesHandler{db: dbClient}
}

func (h *DevicesHandler) Register(r chi.Router) {
	r.Get("/api/v1/sites/{site_id}/devices/{device_type}/{device_id}", h.GetDevice)
	r.Get("/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/sensor", h.GetSensorData)
	r.Get("/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/environment", h.GetEnvironment)
}

func (h *DevicesHandler) GetDevice(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	deviceType := chi.URLParam(r, "device_type")
	deviceID := chi.URLParam(r, "device_id")

	device, err := h.db.GetDeviceStatus(r.Context(), siteID, deviceType, deviceID)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if device == nil {
		writeError(w, http.StatusNotFound, "device not found")
		return
	}
	writeJSON(w, http.StatusOK, device)
}

func (h *DevicesHandler) GetSensorData(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	deviceType := chi.URLParam(r, "device_type")
	deviceID := chi.URLParam(r, "device_id")

	from, to, ok := parseTimeRange(w, r)
	if !ok {
		return
	}
	limit, offset := parsePagination(r)

	rows, err := h.db.QuerySensorData(r.Context(), siteID, deviceType, deviceID, from, to, limit, offset)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id":     siteID,
		"device_type": deviceType,
		"device_id":   deviceID,
		"from":        from.Format(time.RFC3339),
		"to":          to.Format(time.RFC3339),
		"count":       len(rows),
		"data":        rows,
	})
}

func (h *DevicesHandler) GetEnvironment(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	deviceType := chi.URLParam(r, "device_type")
	deviceID := chi.URLParam(r, "device_id")

	from, to, ok := parseTimeRange(w, r)
	if !ok {
		return
	}
	limit, offset := parsePagination(r)

	rows, err := h.db.QueryEnvironment(r.Context(), siteID, deviceType, deviceID, from, to, limit, offset)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id":     siteID,
		"device_type": deviceType,
		"device_id":   deviceID,
		"from":        from.Format(time.RFC3339),
		"to":          to.Format(time.RFC3339),
		"count":       len(rows),
		"data":        rows,
	})
}

func parseTimeRange(w http.ResponseWriter, r *http.Request) (time.Time, time.Time, bool) {
	fromStr := r.URL.Query().Get("from")
	toStr := r.URL.Query().Get("to")

	if fromStr == "" || toStr == "" {
		writeError(w, http.StatusBadRequest, "from and to query parameters are required (RFC3339 format)")
		return time.Time{}, time.Time{}, false
	}

	from, err := time.Parse(time.RFC3339, fromStr)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid 'from' format, use RFC3339 (e.g. 2026-05-26T00:00:00Z)")
		return time.Time{}, time.Time{}, false
	}

	to, err := time.Parse(time.RFC3339, toStr)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid 'to' format, use RFC3339 (e.g. 2026-05-26T23:59:59Z)")
		return time.Time{}, time.Time{}, false
	}

	if from.After(to) {
		writeError(w, http.StatusBadRequest, "'from' must be before 'to'")
		return time.Time{}, time.Time{}, false
	}

	return from, to, true
}

func parsePagination(r *http.Request) (limit, offset int) {
	limit = 20
	if l := r.URL.Query().Get("limit"); l != "" {
		if n, err := strconv.Atoi(l); err == nil && n > 0 {
			limit = n
			if limit > 100 {
				limit = 100
			}
		}
	}
	if o := r.URL.Query().Get("offset"); o != "" {
		if n, err := strconv.Atoi(o); err == nil && n > 0 {
			offset = n
		}
	}
	return
}
