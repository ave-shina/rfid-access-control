package crypto

import "testing"

func TestHashRawUIDNormalizesCase(t *testing.T) {
	lower := HashRawUID("e2a30040d4")
	upper := HashRawUID("E2A30040D4")
	if lower != upper {
		t.Fatalf("case normalization failed: %q != %q", lower, upper)
	}
	if len(lower) != 64 {
		t.Fatalf("expected 64-char hex digest, got %d", len(lower))
	}
}

func TestHashRawUIDTrimsWhitespace(t *testing.T) {
	if HashRawUID(" e2a30040d4 ") != HashRawUID("e2a30040d4") {
		t.Fatal("whitespace should be trimmed before hashing")
	}
}

func TestPepperHashIsDeterministic(t *testing.T) {
	a := PepperHash("abc", "pepper")
	b := PepperHash("abc", "pepper")
	if a != b || len(a) != 64 {
		t.Fatalf("pepper hash must be deterministic 64-char hex, got %q vs %q", a, b)
	}
}
