package dedup

import (
	"testing"
	"time"
)

func TestCache_CheckAndAdd_New(t *testing.T) {
	c := New(5 * time.Second)
	if !c.CheckAndAdd("key1") {
		t.Error("expected new key to be accepted")
	}
	if c.Size() != 1 {
		t.Errorf("expected size=1, got %d", c.Size())
	}
}

func TestCache_CheckAndAdd_Duplicate(t *testing.T) {
	c := New(5 * time.Second)
	c.CheckAndAdd("key1")
	if c.CheckAndAdd("key1") {
		t.Error("expected duplicate key to be rejected")
	}
	if c.Size() != 1 {
		t.Errorf("expected size still 1, got %d", c.Size())
	}
}

func TestCache_CheckAndAdd_DifferentKeys(t *testing.T) {
	c := New(5 * time.Second)
	if !c.CheckAndAdd("key1") {
		t.Error("expected key1 to be accepted")
	}
	if !c.CheckAndAdd("key2") {
		t.Error("expected key2 to be accepted")
	}
	if c.Size() != 2 {
		t.Errorf("expected size=2, got %d", c.Size())
	}
}

func TestCache_PurgeExpired(t *testing.T) {
	c := New(50 * time.Millisecond)
	c.CheckAndAdd("key1")
	c.CheckAndAdd("key2")

	time.Sleep(100 * time.Millisecond)
	c.CheckAndAdd("key3") // key3 is fresh

	removed := c.PurgeExpired()
	if removed != 2 {
		t.Errorf("expected 2 expired entries removed, got %d", removed)
	}
	if c.Size() != 1 {
		t.Errorf("expected size=1 after purge, got %d", c.Size())
	}
	if !c.CheckAndAdd("key1") {
		t.Error("expected expired key1 to be accepted again as new")
	}
}

func TestMakeKey(t *testing.T) {
	key := MakeKey("de01", 12345678, "edgevib/upload/1/vibration")
	expected := "de01|12345678|edgevib/upload/1/vibration"
	if key != expected {
		t.Errorf("expected %q, got %q", expected, key)
	}
}

func TestMakeKey_DifferentDeviceID(t *testing.T) {
	k1 := MakeKey("de01", 12345, "edgevib/upload/1/vibration")
	k2 := MakeKey("nde01", 12345, "edgevib/upload/2/vibration")
	if k1 == k2 {
		t.Error("expected different keys for different device_ids")
	}
}

func TestMakeKey_DifferentTimestamp(t *testing.T) {
	k1 := MakeKey("de01", 12345, "edgevib/upload/1/vibration")
	k2 := MakeKey("de01", 12346, "edgevib/upload/1/vibration")
	if k1 == k2 {
		t.Error("expected different keys for different timestamps")
	}
}

func TestMakeKey_DifferentTopic(t *testing.T) {
	k1 := MakeKey("de01", 12345, "edgevib/upload/1/vibration")
	k2 := MakeKey("de01", 12345, "edgevib/upload/1/environment")
	if k1 == k2 {
		t.Error("expected different keys for different topics")
	}
}

func TestCache_ConcurrentAccess(t *testing.T) {
	c := New(5 * time.Second)
	done := make(chan bool)

	for i := 0; i < 10; i++ {
		go func(id int) {
			for j := 0; j < 100; j++ {
				c.CheckAndAdd(MakeKey("de01", int64(j), "topic"))
			}
			done <- true
		}(i)
	}

	for i := 0; i < 10; i++ {
		<-done
	}
	// No race detector: just ensure no panics
}
