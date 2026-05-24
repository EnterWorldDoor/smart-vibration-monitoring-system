package dedup

import (
	"fmt"
	"sync"
	"time"
)

// Cache is a thread-safe in-memory deduplication cache.
type Cache struct {
	mu    sync.Mutex
	items map[string]time.Time
	ttl   time.Duration
}

// New creates a new dedup cache with the given TTL.
func New(ttl time.Duration) *Cache {
	return &Cache{
		items: make(map[string]time.Time),
		ttl:   ttl,
	}
}

// CheckAndAdd returns true if the key is new (not a duplicate).
// The key is added to the cache. Returns false for duplicates.
func (c *Cache) CheckAndAdd(key string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	if _, exists := c.items[key]; exists {
		return false
	}
	c.items[key] = time.Now()
	return true
}

// PurgeExpired removes all entries older than TTL.
func (c *Cache) PurgeExpired() int {
	c.mu.Lock()
	defer c.mu.Unlock()

	cutoff := time.Now().Add(-c.ttl)
	removed := 0
	for k, t := range c.items {
		if t.Before(cutoff) {
			delete(c.items, k)
			removed++
		}
	}
	return removed
}

// Size returns the current number of entries.
func (c *Cache) Size() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return len(c.items)
}

// MakeKey builds a dedup key from device_id, timestamp_ms, and topic.
func MakeKey(deviceID string, timestampMS int64, topic string) string {
	return fmt.Sprintf("%s|%d|%s", deviceID, timestampMS, topic)
}
