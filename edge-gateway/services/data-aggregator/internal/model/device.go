package model

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// DeviceRegistry maps ESP32 uint8 dev_id to (site_id, device_type, device_id).
type DeviceRegistry struct {
	devices       map[uint8]DeviceEntry
	defaultSiteID string
}

// devicesFile is the YAML structure for config/devices.yaml
type devicesFile struct {
	Devices []DeviceEntry `yaml:"devices"`
}

// LoadDeviceRegistry reads and parses the devices YAML file.
func LoadDeviceRegistry(path string, defaultSiteID string) (*DeviceRegistry, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read devices registry: %w", err)
	}
	var df devicesFile
	if err := yaml.Unmarshal(data, &df); err != nil {
		return nil, fmt.Errorf("parse devices registry: %w", err)
	}
	r := &DeviceRegistry{
		devices:       make(map[uint8]DeviceEntry, len(df.Devices)),
		defaultSiteID: defaultSiteID,
	}
	for _, d := range df.Devices {
		r.devices[d.DevID] = d
	}
	return r, nil
}

// Lookup returns (siteID, deviceType, deviceID) for a dev_id.
// Falls back to default values when dev_id is unknown.
func (r *DeviceRegistry) Lookup(devID uint8) (string, string, string) {
	if entry, ok := r.devices[devID]; ok {
		return entry.SiteID, entry.DeviceType, entry.DeviceID
	}
	return r.defaultSiteID, "unknown", fmt.Sprintf("dev-%d", devID)
}
