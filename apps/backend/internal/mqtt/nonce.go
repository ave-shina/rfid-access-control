package mqtt

import (
	"sync"
	"time"
)

// NonceCache provides thread-safe anti-replay protection with TTL-based expiry.
type NonceCache struct {
	mu      sync.RWMutex
	entries map[string]time.Time
	ttl     time.Duration
}

// NewNonceCache creates a cache that rejects duplicate nonces within the TTL window.
func NewNonceCache(ttl time.Duration) *NonceCache {
	nc := &NonceCache{
		entries: make(map[string]time.Time),
		ttl:     ttl,
	}
	go nc.cleanup()
	return nc
}

// CheckAndStore returns true if the nonce is fresh (not seen recently), and stores it.
// Returns false if the nonce was already seen within the TTL window.
func (nc *NonceCache) CheckAndStore(nonce string) bool {
	nc.mu.Lock()
	defer nc.mu.Unlock()

	if exp, exists := nc.entries[nonce]; exists && time.Since(exp.Add(-nc.ttl)) < nc.ttl {
		return false
	}
	nc.entries[nonce] = time.Now()
	return true
}

// cleanup removes expired entries every 30 seconds.
func (nc *NonceCache) cleanup() {
	ticker := time.NewTicker(30 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		nc.mu.Lock()
		now := time.Now()
		for k, t := range nc.entries {
			if now.Sub(t) > nc.ttl {
				delete(nc.entries, k)
			}
		}
		nc.mu.Unlock()
	}
}
