package topic

import "testing"

func TestParseTopic_PatternA_Train(t *testing.T) {
	r, err := ParseTopic("edgevib/train/1/vibration")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.Pattern != "A" {
		t.Errorf("expected Pattern A, got %q", r.Pattern)
	}
	if r.DevID != 1 {
		t.Errorf("expected dev_id=1, got %d", r.DevID)
	}
	if r.DataType != "vibration" {
		t.Errorf("expected data_type=vibration, got %q", r.DataType)
	}
}

func TestParseTopic_PatternA_UploadVibration(t *testing.T) {
	r, err := ParseTopic("edgevib/upload/5/vibration")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.Pattern != "A" {
		t.Errorf("expected Pattern A, got %q", r.Pattern)
	}
	if r.DevID != 5 {
		t.Errorf("expected dev_id=5, got %d", r.DevID)
	}
	if r.DataType != "vibration" {
		t.Errorf("expected data_type=vibration, got %q", r.DataType)
	}
}

func TestParseTopic_PatternA_Health(t *testing.T) {
	r, err := ParseTopic("edgevib/upload/1/health")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.DataType != "health" {
		t.Errorf("expected data_type=health, got %q", r.DataType)
	}
}

func TestParseTopic_PatternA_Status(t *testing.T) {
	r, err := ParseTopic("edgevib/upload/3/status")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.DevID != 3 {
		t.Errorf("expected dev_id=3, got %d", r.DevID)
	}
	if r.DataType != "status" {
		t.Errorf("expected data_type=status, got %q", r.DataType)
	}
}

func TestParseTopic_PatternA_AIHealth(t *testing.T) {
	r, err := ParseTopic("edgevib/1/health/ai")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.Pattern != "A" {
		t.Errorf("expected Pattern A, got %q", r.Pattern)
	}
	if r.DevID != 1 {
		t.Errorf("expected dev_id=1, got %d", r.DevID)
	}
	if r.DataType != "health/ai" {
		t.Errorf("expected data_type=health/ai, got %q", r.DataType)
	}
}

func TestParseTopic_PatternB_Full(t *testing.T) {
	r, err := ParseTopic("EdgeVib/factory1/motor/de01/data/sensor")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.Pattern != "B" {
		t.Errorf("expected Pattern B, got %q", r.Pattern)
	}
	if r.SiteID != "factory1" {
		t.Errorf("expected site_id=factory1, got %q", r.SiteID)
	}
	if r.DeviceType != "motor" {
		t.Errorf("expected device_type=motor, got %q", r.DeviceType)
	}
	if r.DeviceID != "de01" {
		t.Errorf("expected device_id=de01, got %q", r.DeviceID)
	}
	if r.DataType != "sensor" {
		t.Errorf("expected data_type=sensor, got %q", r.DataType)
	}
}

func TestParseTopic_PatternB_StatusHealth(t *testing.T) {
	r, err := ParseTopic("EdgeVib/factory1/gateway/esp32-01/status/health")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.DeviceType != "gateway" {
		t.Errorf("expected device_type=gateway, got %q", r.DeviceType)
	}
	if r.DeviceID != "esp32-01" {
		t.Errorf("expected device_id=esp32-01, got %q", r.DeviceID)
	}
	if r.DataType != "health" {
		t.Errorf("expected data_type=health, got %q", r.DataType)
	}
}

func TestParseTopic_PatternB_LongDataType(t *testing.T) {
	r, err := ParseTopic("EdgeVib/factory1/camera/vision01/data/image/jpg")
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if r.DataType != "image/jpg" {
		t.Errorf("expected data_type=image/jpg, got %q", r.DataType)
	}
}

func TestParseTopic_Invalid_TooShort(t *testing.T) {
	_, err := ParseTopic("edgevib")
	if err == nil {
		t.Error("expected error for too-short topic")
	}
}

func TestParseTopic_Invalid_WrongPrefix(t *testing.T) {
	_, err := ParseTopic("mqtt/upload/1/vibration")
	if err == nil {
		t.Error("expected error for unknown prefix")
	}
}

func TestParseTopic_Invalid_BadDevID(t *testing.T) {
	_, err := ParseTopic("edgevib/train/abc/vibration")
	if err == nil {
		t.Error("expected error for non-numeric dev_id")
	}
}

func TestParseTopic_Invalid_UnknownMode(t *testing.T) {
	_, err := ParseTopic("edgevib/invalid/1/vibration")
	if err == nil {
		t.Error("expected error for unknown mode")
	}
}

func TestParseTopic_PatternB_TooShort(t *testing.T) {
	_, err := ParseTopic("EdgeVib/a/b/c")
	if err == nil {
		t.Error("expected error for Pattern B with too few levels")
	}
}
