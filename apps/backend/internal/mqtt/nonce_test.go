package mqtt

import (
	"testing"
	"time"
)

func TestNonceRejectsReplay(t *testing.T) {
	nc := NewNonceCache(60 * time.Second)

	if !nc.CheckAndStore("abc123") {
		t.Fatal("first use of nonce should be accepted")
	}
	if nc.CheckAndStore("abc123") {
		t.Fatal("replayed nonce should be rejected")
	}
}

func TestNonceExpiresAfterTTL(t *testing.T) {
	nc := NewNonceCache(50 * time.Millisecond)

	if !nc.CheckAndStore("xyz789") {
		t.Fatal("first use of nonce should be accepted")
	}
	time.Sleep(80 * time.Millisecond)
	if !nc.CheckAndStore("xyz789") {
		t.Fatal("nonce should be accepted again after TTL expiry")
	}
}
