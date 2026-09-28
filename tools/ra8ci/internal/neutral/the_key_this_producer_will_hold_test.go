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
)

// What a producer will hold a signing key for. The observer refusals are
// already pinned in TestProducerRequiresRealObserverAndFailsClosedOnUnsafeState;
// these take the key material and the clock beside it, because a producer
// built on a key it cannot sign with is a board agent that discovers it
// cannot answer only once a recovery is already waiting on it.

func testPrivate(t *testing.T) ed25519.PrivateKey {
	t.Helper()
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return private
}

func TestNewProducerRefusesKeyMaterialItCannotSignWith(t *testing.T) {
	private := testPrivate(t)
	for name, key := range map[string]ed25519.PrivateKey{
		"no key at all":       nil,
		"an empty key":        {},
		"a seed, not a key":   private[:ed25519.SeedSize],
		"one byte short":      private[:ed25519.PrivateKeySize-1],
		"one byte too long":   append(append(ed25519.PrivateKey(nil), private...), 0),
		"a public key's size": ed25519.PrivateKey(make([]byte, ed25519.PublicKeySize)),
	} {
		if _, err := NewProducer("board-agent-1", key, &fakeObserver{}, nil); !errors.Is(err, ErrObservationAbsent) {
			t.Errorf("%s = %v", name, err)
		}
	}
}

// A key ID is what the verifier's allowlist is keyed by, so a producer will
// not carry one the allowlist could never name.
func TestNewProducerHoldsTheKeyIDToItsShape(t *testing.T) {
	private := testPrivate(t)
	for name, id := range map[string]string{
		"no ID":            "",
		"a space":          "board agent 1",
		"a slash":          "board/agent",
		"a colon":          "board:agent",
		"one rune too far": strings.Repeat("k", 65),
	} {
		if _, err := NewProducer(id, private, &fakeObserver{}, nil); !errors.Is(err, ErrObservationAbsent) {
			t.Errorf("%s = %v", name, err)
		}
	}

	for name, id := range map[string]string{
		"the punctuation it allows": "board-agent_1.rev2",
		"a single rune":             "k",
		"exactly at the bound":      strings.Repeat("k", 64),
	} {
		if _, err := NewProducer(id, private, &fakeObserver{}, nil); err != nil {
			t.Errorf("%s = %v", name, err)
		}
	}
}

// The key is copied in, so a caller that reuses or wipes its own buffer
// afterwards cannot change what this producer signs with.
func TestNewProducerKeepsItsOwnCopyOfTheKey(t *testing.T) {
	private := testPrivate(t)
	challenge := challengeFixture()
	observer := &fakeObserver{observation: observationFixture(challenge)}
	producer, err := NewProducer("board-agent-1", private, observer, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	for i := range private {
		private[i] = 0
	}
	if _, err := producer.ProduceNeutralReceipt(context.Background(), challenge); err != nil {
		t.Fatalf("a wiped caller buffer reached the producer: %v", err)
	}
}

// No clock given means the real one, which is what a board agent built from
// configuration gets. A challenge minted around the fixed test clock is then
// not live, and the producer says so rather than signing against a clock it
// was never given.
func TestNewProducerFallsBackToTheRealClock(t *testing.T) {
	private := testPrivate(t)
	stale := challengeFixture()
	producer, err := NewProducer("board-agent-1", private,
		&fakeObserver{observation: observationFixture(stale)}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := producer.ProduceNeutralReceipt(context.Background(), stale); !errors.Is(err, ErrExpired) {
		t.Fatalf("a challenge from the test clock = %v, want ErrExpired", err)
	}

	live := challengeFixture()
	live.IssuedAt = time.Now().UTC().Add(-time.Second)
	live.ExpiresAt = live.IssuedAt.Add(20 * time.Second)
	observation := observationFixture(live)
	observation.ObservedAt = time.Now().UTC().Add(-time.Second)
	liveProducer, err := NewProducer("board-agent-1", private, &fakeObserver{observation: observation}, nil)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := liveProducer.ProduceNeutralReceipt(context.Background(), live)
	if err != nil {
		t.Fatalf("a live challenge on the real clock: %v", err)
	}
	if len(raw) == 0 || len(raw) > MaxReceiptBytes {
		t.Fatalf("receipt of %d bytes", len(raw))
	}
}
