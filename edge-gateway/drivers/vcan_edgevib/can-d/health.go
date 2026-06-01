package main

import (
	"encoding/json"
	"log/slog"
	"os"
	"strconv"
	"strings"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

const healthInterval = 30 * time.Second

/* healthReport is the JSON published every 30s to MQTT */
type healthReport struct {
	Service    string `json:"service"`
	Status     string `json:"status"`
	FramesRx   uint64 `json:"frames_rx"`
	FramesTx   uint64 `json:"frames_tx"`
	CrcErrors  uint64 `json:"crc_errors"`
	KernelCrc  uint32 `json:"kernel_crc_errors"`
	KernelFifo uint32 `json:"kernel_fifo_overruns"`
	UptimeSec  uint64 `json:"uptime_s"`
}

func readSysfsUint32(path string) uint32 {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	v, err := strconv.ParseUint(strings.TrimSpace(string(data)), 10, 32)
	if err != nil {
		return 0
	}
	return uint32(v)
}

/* RunHealthLoop periodically publishes health telemetry via MQTT */
func RunHealthLoop(brokerURL, clientID, topic string, mc *MQTTClient, logger *slog.Logger) {
	/* Separate MQTT connection for health (simpler lifecycle) */
	opts := mqtt.NewClientOptions().
		AddBroker(brokerURL).
		SetClientID(clientID + "-health").
		SetAutoReconnect(true).
		SetCleanSession(true).
		SetKeepAlive(60 * time.Second)

	client := mqtt.NewClient(opts)
	if token := client.Connect(); !token.WaitTimeout(10*time.Second) || token.Error() != nil {
		logger.Warn("Health MQTT connect failed, retrying in background")
	}
	defer client.Disconnect(250)

	var startTime uint64

	ticker := time.NewTicker(healthInterval)
	defer ticker.Stop()

	for range ticker.C {
		startTime++
		framesRx, framesTx, crcErrors := mc.Stats()

		report := healthReport{
			Service:    "edgevib-can-d",
			Status:     "running",
			FramesRx:   framesRx,
			FramesTx:   framesTx,
			CrcErrors:  crcErrors,
			KernelCrc:  readSysfsUint32("/sys/class/net/vcan_edgevib/device/crc_errors"),
			KernelFifo: readSysfsUint32("/sys/class/net/vcan_edgevib/device/fifo_overruns"),
			UptimeSec:  startTime * uint64(healthInterval.Seconds()),
		}

		payload, err := json.Marshal(report)
		if err != nil {
			logger.Error("Health JSON marshal error", "err", err)
			continue
		}

		token := client.Publish(topic, 1, false, payload)
		if token.WaitTimeout(5*time.Second) && token.Error() != nil {
			logger.Warn("Health publish error", "err", token.Error())
		}
	}
}
