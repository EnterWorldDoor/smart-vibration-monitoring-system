package handler

import (
	"encoding/csv"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"edgevib/api-server/internal/db"

	"github.com/go-chi/chi/v5"
)

// ExportHandler serves the training data export endpoint.
type ExportHandler struct {
	db *db.Client
}

// NewExportHandler creates a new export handler.
func NewExportHandler(dbClient *db.Client) *ExportHandler {
	return &ExportHandler{db: dbClient}
}

// Register registers export routes on the router.
func (h *ExportHandler) Register(r chi.Router) {
	r.Get("/api/v1/data/export", h.ExportCSV)
}

// ExportCSV streams training data as CSV.
func (h *ExportHandler) ExportCSV(w http.ResponseWriter, r *http.Request) {
	// Parse time range
	fromStr := r.URL.Query().Get("from")
	toStr := r.URL.Query().Get("to")

	if fromStr == "" || toStr == "" {
		writeError(w, http.StatusBadRequest, "query parameters 'from' and 'to' are required (RFC3339 format)")
		return
	}

	from, err := time.Parse(time.RFC3339, fromStr)
	if err != nil {
		writeError(w, http.StatusBadRequest, fmt.Sprintf("invalid 'from' time format: %v", err))
		return
	}
	to, err := time.Parse(time.RFC3339, toStr)
	if err != nil {
		writeError(w, http.StatusBadRequest, fmt.Sprintf("invalid 'to' time format: %v", err))
		return
	}
	if from.After(to) {
		writeError(w, http.StatusBadRequest, "'from' must be before 'to'")
		return
	}

	// Parse format param (only csv supported)
	format := r.URL.Query().Get("format")
	if format == "" {
		format = "csv"
	}
	if format != "csv" {
		writeError(w, http.StatusBadRequest, "only format=csv is supported")
		return
	}

	// Parse optional filters
	params := db.ExportParams{
		From: from,
		To:   to,
	}

	if sitesStr := r.URL.Query().Get("sites"); sitesStr != "" {
		for _, s := range strings.Split(sitesStr, ",") {
			s = strings.TrimSpace(s)
			if s != "" {
				params.Sites = append(params.Sites, s)
			}
		}
	}
	if devicesStr := r.URL.Query().Get("devices"); devicesStr != "" {
		for _, d := range strings.Split(devicesStr, ",") {
			d = strings.TrimSpace(d)
			if d != "" {
				params.Devices = append(params.Devices, d)
			}
		}
	}

	// Query data
	rows, err := h.db.QueryExportData(r.Context(), params)
	if err != nil {
		writeError(w, http.StatusInternalServerError, fmt.Sprintf("query failed: %v", err))
		return
	}

	// Write CSV response
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", "attachment; filename=\"training_data.csv\"")
	w.WriteHeader(http.StatusOK)

	writer := csv.NewWriter(w)

	// Header — legacy CSV format, aligned with edge-ai training_data.csv
	header := []string{
		"timestamp_ms", "dev_id",
		"rms_x", "rms_y", "rms_z", "overall_rms",
		"peak_freq", "peak_amp",
		"temperature_c", "humidity_rh",
		"label",
	}
	if err := writer.Write(header); err != nil {
		return
	}

	// Data rows
	for _, row := range rows {
		record := []string{
			strconv.FormatInt(row.Time.UnixMilli(), 10),
			row.DeviceID,
			floatOrEmpty(row.RMSX),
			floatOrEmpty(row.RMSY),
			floatOrEmpty(row.RMSZ),
			floatOrEmpty(row.OverallRMS),
			floatOrEmpty(row.PeakFreq),
			floatOrEmpty(row.PeakAmp),
			floatOrEmpty(row.TemperatureC),
			floatOrEmpty(row.HumidityRH),
			"unknown",
		}
		if err := writer.Write(record); err != nil {
			return
		}
	}
	writer.Flush()
	if err := writer.Error(); err != nil {
		return
	}
}

func floatOrEmpty(f *float64) string {
	if f == nil {
		return ""
	}
	return strconv.FormatFloat(*f, 'f', -1, 64)
}
