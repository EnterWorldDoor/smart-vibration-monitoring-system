/*
 * gpio_ctrl.go — gpiolib cdev interaction via os/exec subprocess
 *
 * Uses the gpiod command-line tools (apt install gpiod) for:
 *   - gpioset: set output line value
 *   - gpiomon: monitor input lines for IRQ events
 *   - heartbeat: periodic line flip goroutine
 *
 * This is a deliberate design choice: D3 GPIO events are very low frequency
 * (heartbeat 2Hz, alerts a few times/year, IRQ events sub-second).
 * os/exec is simpler, safer, and more debuggable than ioctl or cgo.
 */

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"os/exec"
	"time"
)

/* ---- Data types ---- */

// GPIOAction describes a requested GPIO output change
type GPIOAction struct {
	Line  int
	Value int // 0 or 1
	Label string
}

// DaemonStats holds runtime statistics for health reporting
type DaemonStats struct {
	Actions   int       `json:"actions"`
	Errors    int       `json:"errors"`
	IRQEvents int       `json:"irq_events"`
	StartTime time.Time `json:"-"`
}

// gpiomon JSON output format (gpiod v2.x)
type gpioMonEvent struct {
	Event    string `json:"event"`    // "FALLING_EDGE" or "RISING_EDGE"
	Line     int    `json:"line"`
	Sequence int    `json:"seqno"`
}

/* ---- GPIO output ---- */

func SetOutput(chip string, line int, value int) error {
	arg := fmt.Sprintf("%d=%d", line, value)
	cmd := exec.Command("gpioset", chip, arg)
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("gpioset %s %s: %w", chip, arg, err)
	}
	return nil
}

func applyAction(chip string, action GPIOAction) error {
	slog.Debug("gpio action", "line", action.Line, "value", action.Value, "label", action.Label)
	return SetOutput(chip, action.Line, action.Value)
}

/* ---- Heartbeat ---- */

func runHeartbeat(ctx context.Context, chip string, line int, intervalMS int, logger *slog.Logger) {
	ticker := time.NewTicker(time.Duration(intervalMS) * time.Millisecond)
	defer ticker.Stop()

	value := 0
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			value = 1 - value // flip
			if err := SetOutput(chip, line, value); err != nil {
				logger.Warn("heartbeat set failed", "err", err)
			}
		}
	}
}

/* ---- IRQ monitoring ---- */

// runIRQMonitor starts gpiomon subprocess and calls the MQTT publisher on events
func runIRQMonitor(ctx context.Context, chip string, lines []int, cfg *Config, logger *slog.Logger) {
	args := []string{"-r", chip}
	for _, l := range lines {
		args = append(args, fmt.Sprintf("%d", l))
	}

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		cmd := exec.Command("gpiomon", args...)
		stdout, err := cmd.StdoutPipe()
		if err != nil {
			logger.Error("gpiomon pipe failed", "err", err)
			select {
			case <-ctx.Done():
				return
			case <-time.After(5 * time.Second):
			}
			continue
		}

		if err := cmd.Start(); err != nil {
			logger.Error("gpiomon start failed", "err", err)
			select {
			case <-ctx.Done():
				return
			case <-time.After(5 * time.Second):
			}
			continue
		}
		logger.Info("gpiomon started", "chip", chip, "lines", lines)

		scanner := bufio.NewScanner(stdout)
		for scanner.Scan() {
			var evt gpioMonEvent
			if err := json.Unmarshal(scanner.Bytes(), &evt); err != nil {
				logger.Warn("gpiomon parse failed", "line", scanner.Text(), "err", err)
				continue
			}

			logger.Info("IRQ event", "line", evt.Line, "event", evt.Event, "seq", evt.Sequence)

			// Publish emergency event via MQTT
			broker := fmt.Sprintf("tcp://%s:%d", cfg.MQTT.Broker, cfg.MQTT.Port)
			if evt.Line == cfg.GPIO.Lines.EstopMaster {
				publishEvent(broker, cfg.MQTT.PublishEstopTopic,
					map[string]interface{}{
						"event":  evt.Event,
						"line":   evt.Line,
						"action": "shutdown",
					})
			} else if evt.Line == cfg.GPIO.Lines.PSUFailIn {
				publishEvent(broker, cfg.MQTT.PublishPSUTopic,
					map[string]interface{}{
						"event":               evt.Event,
						"line":                evt.Line,
						"graceful_shutdown_ms": 30000,
					})
			}
		}

		if ctx.Err() != nil {
			return
		}
		logger.Warn("gpiomon exited unexpectedly, restarting in 5s")
		select {
		case <-ctx.Done():
			return
		case <-time.After(5 * time.Second):
		}
	}
}
