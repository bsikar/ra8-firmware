// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// MaxReceiptBytes is read at both ends: the verifier refuses anything longer
// before it looks at a signature, and the producer refuses to emit one. The
// producer's half is the one that matters here, because a challenge can carry
// it there. Two of a challenge's fields, the fixture revision and the restore
// policy, are held only to being non-empty, so a board carrying a long
// revision string is the ordinary way a payload grows past the bound.
//
// Without the producer's check the agent would sign a receipt, hand it over,
// and have it refused at the plane for a length nobody on the board ever
// measured. Refusing locally makes it the signing host's own finding.

// producerFor builds a producer around one challenge's matching observation,
// signing at testNow.
func producerFor(t *testing.T, c store.NeutralChallenge) (*Producer, ed25519.PublicKey) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	producer, err := NewProducer("board-agent-1", private, &fakeObserver{observation: observationFixture(c)},
		func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	return producer, public
}

func TestAReceiptLargerThanAVerifierAcceptsIsNotEmitted(t *testing.T) {
	c := challengeFixture()
	c.FixtureRevision = strings.Repeat("r", MaxReceiptBytes)
	producer, _ := producerFor(t, c)
	raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
	if !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("err = %v, want ErrInvalidReceipt", err)
	}
	if raw != nil {
		t.Fatalf("a refused receipt was still handed back: %d bytes", len(raw))
	}
}

// The refusal is about the size and not about a long field, so a challenge
// whose revision is long but still fits is signed and verifies. This is the
// pair that keeps the bound from quietly becoming a shorter one.
func TestALongChallengeInsideTheBoundIsStillSignedAndVerifies(t *testing.T) {
	c := challengeFixture()
	c.FixtureRevision = strings.Repeat("r", 60000)
	producer, public := producerFor(t, c)
	raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
	if err != nil {
		t.Fatalf("a receipt inside the bound was refused: %v", err)
	}
	if len(raw) <= 60000 || len(raw) > MaxReceiptBytes {
		t.Fatalf("receipt is %d bytes, want the long revision carried and the bound respected", len(raw))
	}
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
		t.Fatalf("a receipt inside the bound did not verify: %v", err)
	}
}
