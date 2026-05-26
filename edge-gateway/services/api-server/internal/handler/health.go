package handler

import (
	"encoding/json"
	"net/http"

	"edgevib/api-server/internal/db"
)

func Health(dbClient *db.Client, mqttConnected func() bool) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		dbStatus := "ok"
		if err := dbClient.Ping(r.Context()); err != nil {
			dbStatus = "error"
		}

		mqttStatus := "ok"
		if !mqttConnected() {
			mqttStatus = "error"
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{
			"db":   dbStatus,
			"mqtt": mqttStatus,
		})
	}
}
