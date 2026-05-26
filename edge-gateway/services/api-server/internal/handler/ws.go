package handler

import (
	"net/http"

	"edgevib/api-server/internal/ws"

	"github.com/go-chi/chi/v5"
	gorillaWS "github.com/gorilla/websocket"
)

var upgrader = gorillaWS.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	CheckOrigin: func(r *http.Request) bool {
		return true // allow all origins on internal network
	},
}

type WSHandler struct {
	hub *ws.Hub
}

func NewWSHandler(hub *ws.Hub) *WSHandler {
	return &WSHandler{hub: hub}
}

func (h *WSHandler) Register(r chi.Router) {
	r.Get("/api/v1/ws/events", h.ServeWS)
}

func (h *WSHandler) ServeWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}

	client := ws.NewClient(h.hub, conn)
	h.hub.Register <- client
	go client.WritePump()
	go client.ReadPump()
}
