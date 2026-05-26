package handler

import (
	"net/http"
	"strconv"
	"time"

	"edgevib/api-server/internal/db"

	"github.com/go-chi/chi/v5"
)

type ReportsHandler struct {
	db *db.Client
}

func NewReportsHandler(dbClient *db.Client) *ReportsHandler {
	return &ReportsHandler{db: dbClient}
}

func (h *ReportsHandler) Register(r chi.Router) {
	r.Get("/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/ai-reports", h.GetAIReports)
	r.Get("/api/v1/sites/{site_id}/devices/{device_type}/{device_id}/llm-reports", h.GetLLMReports)
}

func (h *ReportsHandler) GetAIReports(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	deviceID := chi.URLParam(r, "device_id")

	from, to, ok := parseReportTimeRange(w, r)
	if !ok {
		return
	}

	severity := db.BuildSeverityFilter(r.URL.Query().Get("severity"))
	if r.URL.Query().Get("severity") != "" && severity == "" {
		writeError(w, http.StatusBadRequest, "invalid severity, allowed: NORMAL, WARNING, CRITICAL")
		return
	}

	limit := parseReportLimit(r)

	reports, err := h.db.QueryAIReports(r.Context(), siteID, deviceID, from, to, severity, limit)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id":   siteID,
		"device_id": deviceID,
		"from":      from.Format(time.RFC3339),
		"to":        to.Format(time.RFC3339),
		"count":     len(reports),
		"data":      reports,
	})
}

func (h *ReportsHandler) GetLLMReports(w http.ResponseWriter, r *http.Request) {
	siteID := chi.URLParam(r, "site_id")
	deviceID := chi.URLParam(r, "device_id")

	from, to, ok := parseReportTimeRange(w, r)
	if !ok {
		return
	}

	severity := db.BuildSeverityFilter(r.URL.Query().Get("severity"))
	if r.URL.Query().Get("severity") != "" && severity == "" {
		writeError(w, http.StatusBadRequest, "invalid severity, allowed: NORMAL, WARNING, CRITICAL")
		return
	}

	limit := parseReportLimit(r)

	reports, err := h.db.QueryLLMReports(r.Context(), siteID, deviceID, from, to, severity, limit)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"site_id":   siteID,
		"device_id": deviceID,
		"from":      from.Format(time.RFC3339),
		"to":        to.Format(time.RFC3339),
		"count":     len(reports),
		"data":      reports,
	})
}

func parseReportTimeRange(w http.ResponseWriter, r *http.Request) (time.Time, time.Time, bool) {
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

func parseReportLimit(r *http.Request) int {
	limit := 20
	if l := r.URL.Query().Get("limit"); l != "" {
		if n, err := strconv.Atoi(l); err == nil && n > 0 {
			limit = n
			if limit > 100 {
				limit = 100
			}
		}
	}
	return limit
}
