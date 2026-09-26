package neutral

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// longChallenge lives the full maxChallengeAge and was issued long enough ago
// that an observation can be far older than maxObservationAge while still
// sitting inside the challenge window. That is exactly the shape the verifier
// used to accept.
func longChallenge() store.NeutralChallenge {
	c := challengeFixture()
	c.IssuedAt = testNow.Add(-25 * time.Second)
	c.ExpiresAt = testNow.Add(5 * time.Second)
	return c
}

// signedFor produces one genuine receipt for the given challenge, signed at
// testNow, and returns the key pair so a test can re-sign an altered payload.
func signedFor(t *testing.T, c store.NeutralChallenge) (ed25519.PublicKey, ed25519.PrivateKey, []byte) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	observation := observationFixture(c)
	observation.ObservedAt = testNow.Add(-time.Second)
	producer, err := NewProducer("board-agent-1", private, &fakeObserver{observation: observation},
		func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
	if err != nil {
		t.Fatal(err)
	}
	return public, private, raw
}

// resign rewrites the payload and signs it with the producer's own key, so the
// receipt under test is cryptographically valid and only its claims differ.
func resign(t *testing.T, raw []byte, private ed25519.PrivateKey, mutate func(*Payload)) []byte {
	t.Helper()
	var receipt Receipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		t.Fatal(err)
	}
	mutate(&receipt.Payload)
	signed, err := signedBytes(receipt.Payload)
	if err != nil {
		t.Fatal(err)
	}
	receipt.Signature = ed25519.Sign(private, signed)
	encoded, err := json.Marshal(receipt)
	if err != nil {
		t.Fatal(err)
	}
	return encoded
}

// The gap this closes: a challenge may live up to maxChallengeAge, so an
// observation signed that much later passed on the challenge window alone,
// six times further from its signature than the producer would ever sign.
func TestAnObservationOlderThanTheProducerWouldSignIsRefused(t *testing.T) {
	c := longChallenge()
	public, private, raw := signedFor(t, c)
	stale := testNow.Add(-10 * time.Second)
	if stale.Before(c.IssuedAt) || !stale.Before(c.ExpiresAt) || !stale.Before(testNow) {
		t.Fatal("fixture does not place the stale stamp inside the live challenge window")
	}
	altered := resign(t, raw, private, func(p *Payload) { p.ObservedAt = formatTime(stale) })
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, altered); !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("observation %v before its signature accepted: %v", testNow.Sub(stale), err)
	}
}

// The bound is inclusive on both sides, the same comparison the producer makes.
func TestAnObservationExactlyAtTheBoundIsAccepted(t *testing.T) {
	c := longChallenge()
	observation := observationFixture(c)
	observation.ObservedAt = testNow.Add(-maxObservationAge)
	if !checkObservationIsFresh(observation.ObservedAt, testNow) {
		t.Fatal("an observation exactly maxObservationAge old was refused")
	}
	if !validObservation(c, observation, testNow) {
		t.Fatal("the producer signs this observation; the verifier rule must agree")
	}
	public, private, raw := signedFor(t, c)
	atBound := resign(t, raw, private, func(p *Payload) { p.ObservedAt = formatTime(testNow.Add(-maxObservationAge)) })
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, atBound); err != nil {
		t.Fatalf("receipt at the freshness bound refused: %v", err)
	}
}

// One window, stated once: if the two ever disagree at any age, this fails.
func TestTheProducerAndVerifierShareOneFreshnessWindow(t *testing.T) {
	c := longChallenge()
	for _, age := range []time.Duration{0, time.Second, maxObservationAge - time.Nanosecond, maxObservationAge,
		maxObservationAge + time.Nanosecond, 10 * time.Second, maxChallengeAge - time.Second} {
		observation := observationFixture(c)
		observation.ObservedAt = testNow.Add(-age)
		produced := validObservation(c, observation, testNow)
		verified := checkObservationIsFresh(observation.ObservedAt, testNow)
		if produced != verified {
			t.Fatalf("age %v: producer accepts %v, verifier accepts %v", age, produced, verified)
		}
	}
}

// An observation stamped after its own signature is still refused: that clause
// moved into this rule rather than being dropped.
func TestAnObservationAfterItsSignatureIsRefused(t *testing.T) {
	if checkObservationIsFresh(testNow.Add(time.Nanosecond), testNow) {
		t.Fatal("an observation stamped after the signature was accepted")
	}
	c := longChallenge()
	public, private, raw := signedFor(t, c)
	future := resign(t, raw, private, func(p *Payload) { p.ObservedAt = formatTime(testNow.Add(time.Nanosecond)) })
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, future); !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("observation after the signature accepted: %v", err)
	}
}

// The receipts a producer actually signs still verify, under both challenge
// shapes: the rule did not quietly narrow the challenge window itself.
func TestAFreshlyProducedReceiptStillVerifies(t *testing.T) {
	for name, c := range map[string]store.NeutralChallenge{"fixture": challengeFixture(), "long": longChallenge()} {
		t.Run(name, func(t *testing.T) {
			public, _, raw := signedFor(t, c)
			if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
				t.Fatalf("a receipt this producer signed was refused: %v", err)
			}
		})
	}
	if longChallenge().ExpiresAt.Sub(longChallenge().IssuedAt) <= maxObservationAge {
		t.Fatal("the long fixture is too short to exercise the gap this rule closes")
	}
}
