// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A neutral observation is a physical measurement, so it takes real time.
// The producer reads its clock twice for that reason: once before it asks
// the observer, and once after the observer answers. Both readings are
// judged against the same challenge window.
//
// The second reading is the one that matters here. A board that took long
// enough to observe that its challenge expired in the meantime must not be
// handed a signed receipt: the signature would carry a SignedAt outside the
// window the plane issued, and the plane would refuse it anyway. Refusing
// at the producer keeps the board from spending a one-use challenge on a
// receipt that was never going to be accepted.
//
// These cases drive that second reading in both directions, and the
// accepted case beside them keeps the refusal from being read as the
// producer refusing every slow observation.

func producerWatching(t *testing.T, observer NeutralObserver, stamps ...time.Time) *Producer {
	t.Helper()
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	producer, err := NewProducer("board-agent-1", private, observer, steppingClock(stamps...))
	if err != nil {
		t.Fatal(err)
	}
	return producer
}

// The window closed while the observer was working, so the receipt is
// refused as expired rather than signed late.
func TestAChallengeThatExpiresDuringTheObservationIsNotSigned(t *testing.T) {
	challenge := challengeFixture()
	observer := &fakeObserver{observation: observationFixture(challenge)}
	producer := producerWatching(t, observer, testNow, challenge.ExpiresAt)

	receipt, err := producer.ProduceNeutralReceipt(context.Background(), challenge)
	if !errors.Is(err, ErrExpired) {
		t.Fatalf("err = %v, want ErrExpired", err)
	}
	if receipt != nil {
		t.Fatalf("an expired challenge was signed anyway: %q", receipt)
	}
	if observer.called != 1 {
		t.Fatalf("observer called %d times, want exactly 1: the refusal belongs after the observation", observer.called)
	}
}

// Expiry is judged on the instant, not on a margin: the challenge is open
// up to its deadline and closed at it.
func TestTheSecondReadingIsJudgedAtTheDeadlineItself(t *testing.T) {
	challenge := challengeFixture()
	for name, second := range map[string]time.Time{
		"a reading exactly at the deadline":              challenge.ExpiresAt,
		"a reading past the deadline":                    challenge.ExpiresAt.Add(time.Second),
		"a clock that ran backwards past the issue time": challenge.IssuedAt.Add(-time.Second),
	} {
		observer := &fakeObserver{observation: observationFixture(challenge)}
		producer := producerWatching(t, observer, testNow, second)
		if _, err := producer.ProduceNeutralReceipt(context.Background(), challenge); !errors.Is(err, ErrExpired) {
			t.Fatalf("%s: err = %v, want ErrExpired", name, err)
		}
	}

	// One nanosecond inside the deadline the window is still open, so the
	// expiry check passes and the NEXT one decides. It refuses: the
	// observation was taken 26 seconds earlier and the producer will not
	// sign a reading older than its 5 second budget. A challenge whose
	// window outlives that budget therefore cannot be signed near its own
	// deadline, and the two refusals stay tellable apart.
	observer := &fakeObserver{observation: observationFixture(challenge)}
	producer := producerWatching(t, observer, testNow, challenge.ExpiresAt.Add(-time.Nanosecond))
	_, err := producer.ProduceNeutralReceipt(context.Background(), challenge)
	if !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("err = %v, want ErrObservationAbsent: inside the window a stale reading is the refusal", err)
	}
	if errors.Is(err, ErrExpired) {
		t.Fatal("a reading inside the window was reported as expired")
	}
}

// An observation that takes time is ordinary. As long as the window is
// still open when the observer answers, the receipt is produced.
func TestAnObservationThatTookTimeIsStillSignedInsideItsWindow(t *testing.T) {
	challenge := challengeFixture()
	observer := &fakeObserver{observation: observationFixture(challenge)}
	producer := producerWatching(t, observer, testNow, testNow.Add(2*time.Second))

	receipt, err := producer.ProduceNeutralReceipt(context.Background(), challenge)
	if err != nil {
		t.Fatalf("a slow but timely observation was refused: %v", err)
	}
	if len(receipt) == 0 {
		t.Fatal("no receipt was produced")
	}
	payload := payloadOf(t, receipt)
	if payload.SignedAt != formatTime(testNow.Add(2*time.Second)) {
		t.Fatalf("SignedAt = %q, want the SECOND clock reading, not the first", payload.SignedAt)
	}
	if payload.ObservedAt != formatTime(observer.observation.ObservedAt.UTC()) {
		t.Fatalf("ObservedAt = %q, want the observation's own stamp", payload.ObservedAt)
	}
}

// The first reading still refuses a challenge that was already closed when
// the producer was asked, and it does so without spending an observation.
func TestAChallengeAlreadyClosedIsRefusedBeforeTheBoardIsTouched(t *testing.T) {
	challenge := challengeFixture()
	observer := &fakeObserver{observation: observationFixture(challenge)}
	producer := producerWatching(t, observer, challenge.ExpiresAt, testNow)

	if _, err := producer.ProduceNeutralReceipt(context.Background(), challenge); !errors.Is(err, ErrExpired) {
		t.Fatalf("err = %v, want ErrExpired", err)
	}
	if observer.called != 0 {
		t.Fatalf("observer called %d times, want 0: a closed challenge must not reach the board", observer.called)
	}
}

// A challenge that is not one at all is refused on its own terms, and the
// clock is never consulted for a second reading.
func TestAnUnusableChallengeIsRefusedAheadOfTheWindow(t *testing.T) {
	for name, mutate := range map[string]func(*store.NeutralChallenge){
		"no purpose":                  func(c *store.NeutralChallenge) { c.Purpose = "" },
		"a release with no lease":     func(c *store.NeutralChallenge) { c.LeaseID = "" },
		"a deadline before its issue": func(c *store.NeutralChallenge) { c.ExpiresAt = c.IssuedAt.Add(-time.Second) },
	} {
		challenge := challengeFixture()
		mutate(&challenge)
		observer := &fakeObserver{observation: observationFixture(challenge)}
		producer := producerWatching(t, observer, testNow, testNow)
		if _, err := producer.ProduceNeutralReceipt(context.Background(), challenge); !errors.Is(err, ErrInvalidChallenge) {
			t.Fatalf("%s: err = %v, want ErrInvalidChallenge", name, err)
		}
		if observer.called != 0 {
			t.Fatalf("%s: observer was asked anyway", name)
		}
	}
}
