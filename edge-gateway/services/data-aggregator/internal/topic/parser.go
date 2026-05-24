package topic

import (
	"fmt"
	"strconv"
	"strings"
)

// ParseResult holds the parsed topic metadata before device registry lookup.
type ParseResult struct {
	Pattern    string // "A" (ESP32 current) or "B" (future)
	DevID      uint8  // only for Pattern A
	SiteID     string // only for Pattern B
	DeviceType string // only for Pattern B
	DeviceID   string // only for Pattern B (Pattern B text id)
	DataType   string // always set
}

// ParseTopic detects the topic pattern and extracts metadata.
func ParseTopic(topic string) (*ParseResult, error) {
	parts := strings.Split(topic, "/")
	if len(parts) < 3 {
		return nil, fmt.Errorf("topic too short: %q", topic)
	}

	switch parts[0] {
	case "edgevib":
		return parsePatternA(topic, parts[1:])
	case "EdgeVib":
		return parsePatternB(topic, parts[1:])
	default:
		return nil, fmt.Errorf("unknown topic prefix: %q", parts[0])
	}
}

// Pattern A (ESP32 current):
//
//	edgevib/{mode}/{dev_id}/{data_type}          — 3-level (train/upload)
//	edgevib/{dev_id}/health/ai                   — 4-level (no mode prefix)
func parsePatternA(topic string, rest []string) (*ParseResult, error) {
	// 4-level: edgevib/{dev_id}/health/ai
	if len(rest) >= 3 {
		devID, err := strconv.ParseUint(rest[0], 10, 8)
		if err == nil && rest[1] == "health" && rest[2] == "ai" {
			return &ParseResult{
				Pattern:  "A",
				DevID:    uint8(devID),
				DataType: "health/ai",
			}, nil
		}
	}

	// 3-level: edgevib/{mode}/{dev_id}/{data_type}
	if len(rest) >= 3 {
		mode := rest[0]
		if mode != "train" && mode != "upload" {
			return nil, fmt.Errorf("unknown mode %q in topic: %s", mode, topic)
		}
		devID, err := strconv.ParseUint(rest[1], 10, 8)
		if err != nil {
			return nil, fmt.Errorf("invalid dev_id %q in topic: %s", rest[1], topic)
		}
		return &ParseResult{
			Pattern:  "A",
			DevID:    uint8(devID),
			DataType: rest[2],
		}, nil
	}

	return nil, fmt.Errorf("unrecognized edgevib topic pattern: %s", topic)
}

// Pattern B (future devices):
//
//	EdgeVib/{site_id}/{device_type}/{device_id}/data/{data_type}
//	EdgeVib/{site_id}/{device_type}/{device_id}/status/{...}
func parsePatternB(topic string, rest []string) (*ParseResult, error) {
	if len(rest) < 5 {
		return nil, fmt.Errorf("Pattern B topic too short (need >=5 levels after prefix): %s", topic)
	}

	result := &ParseResult{
		Pattern:    "B",
		SiteID:     rest[0],
		DeviceType: rest[1],
		DeviceID:   rest[2],
	}

	// rest[3] is "data" or "status", rest[4:] is the data_type
	if rest[3] == "data" || rest[3] == "status" {
		result.DataType = strings.Join(rest[4:], "/")
	} else {
		result.DataType = rest[3]
	}

	return result, nil
}
