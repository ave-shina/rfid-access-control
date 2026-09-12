package crypto

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
)

// HashRawUID returns the SHA-256 hex digest of a raw UID string.
// The input is trimmed and uppercased so it matches the firmware, which
// hashes the uppercase EPC hex representation of the tag.
func HashRawUID(rawUID string) string {
	raw := strings.ToUpper(strings.TrimSpace(rawUID))
	sum := sha256.Sum256([]byte(raw))
	return hex.EncodeToString(sum[:])
}

// PepperHash applies the server-side pepper to an already-hashed UID and
// returns the resulting SHA-256 hex digest. This is the value stored in the
// database (and used for lookups).
func PepperHash(uidHash, pepper string) string {
	sum := sha256.Sum256([]byte(uidHash + pepper))
	return hex.EncodeToString(sum[:])
}
