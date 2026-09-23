package neutral

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	receiptTestID    = "01996f90-3415-7cfe-8ff1-600058131afd"
	receiptTestLease = "01996f90-3415-7cfe-8ff1-600058131afe"
	receiptTestPlan  = "01996f90-3415-7cfe-8ff1-600058131aff"
)

var testNow = time.Date(2026, 9, 22, 12, 0, 10, 0, time.UTC)

type fakeObserver struct {
	observation Observation
	err         error
	called      int
	seen        store.NeutralChallenge
}

func (o *fakeObserver) ObserveNeutral(_ context.Context, challenge store.NeutralChallenge) (Observation, error) {
	o.called++
	o.seen = challenge
	return o.observation, o.err
}

func challengeFixture() store.NeutralChallenge {
	return store.NeutralChallenge{
		ID: receiptTestID, Nonce: strings.Repeat("a", 64), BoardID: "ek-ra8d2",
		Purpose: "release", LeaseID: receiptTestLease, Generation: 4,
		SnapshotVersion: 21, AgentHighWater: 4, FixtureRevision: "fixture-v3",
		ProfileSHA256: strings.Repeat("b", 64), RestorePolicy: "approved-reset",
		IssuedAt: testNow.Add(-5 * time.Second), ExpiresAt: testNow.Add(25 * time.Second),
	}
}

func observationFixture(c store.NeutralChallenge) Observation {
	return Observation{ChallengeID: c.ID, Nonce: c.Nonce, BoardID: c.BoardID, LeaseID: c.LeaseID, Generation: c.Generation,
		AgentHighWater: c.AgentHighWater, FixtureRevision: c.FixtureRevision,
		ProfileSHA256: c.ProfileSHA256, Neutral: true, Evidence: []byte("power isolated; SWD idle"),
		ObservedAt: testNow.Add(-time.Second)}
}

func receiptFixture(t *testing.T) (store.NeutralChallenge, *fakeObserver, ed25519.PublicKey, ed25519.PrivateKey, []byte) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	c := challengeFixture()
	observer := &fakeObserver{observation: observationFixture(c)}
	producer, err := NewProducer("board-agent-1", private, observer, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
	if err != nil {
		t.Fatal(err)
	}
	return c, observer, public, private, raw
}

func testVerifier(t *testing.T, public ed25519.PublicKey) *Verifier {
	t.Helper()
	verifier, err := NewVerifier(map[string]ed25519.PublicKey{"board-agent-1": public}, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	return verifier
}

func TestRoundTripBindsPhysicalObservationToChallenge(t *testing.T) {
	c, observer, public, _, raw := receiptFixture(t)
	if observer.called != 1 || observer.seen.ID != c.ID {
		t.Fatalf("physical observer was not called for exact challenge: %+v", observer)
	}
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
		t.Fatalf("valid signed observation rejected: %v", err)
	}
	var receipt Receipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		t.Fatal(err)
	}
	if receipt.Payload.EvidenceSHA256 == "" || receipt.Payload.State != "neutral" || receipt.Payload.BoardID != c.BoardID ||
		receipt.Payload.SnapshotVersion != c.SnapshotVersion || len(receipt.Signature) != ed25519.SignatureSize {
		t.Fatalf("receipt omitted proof binding: %+v", receipt.Payload)
	}
}

func TestReceiptBindsEveryPersistedChallengeField(t *testing.T) {
	c, _, public, _, raw := receiptFixture(t)
	verifier := testVerifier(t, public)
	modifications := map[string]func(*store.NeutralChallenge){
		"id":             func(v *store.NeutralChallenge) { v.ID = receiptTestPlan },
		"nonce":          func(v *store.NeutralChallenge) { v.Nonce = strings.Repeat("c", 64) },
		"board":          func(v *store.NeutralChallenge) { v.BoardID = "other-board" },
		"purpose":        func(v *store.NeutralChallenge) { v.Purpose = "recovery"; v.RecoveryPlanID = receiptTestPlan },
		"lease":          func(v *store.NeutralChallenge) { v.LeaseID = receiptTestPlan },
		"generation":     func(v *store.NeutralChallenge) { v.Generation++ },
		"version":        func(v *store.NeutralChallenge) { v.SnapshotVersion++ },
		"high_water":     func(v *store.NeutralChallenge) { v.AgentHighWater++ },
		"fixture":        func(v *store.NeutralChallenge) { v.FixtureRevision = "fixture-v4" },
		"profile":        func(v *store.NeutralChallenge) { v.ProfileSHA256 = strings.Repeat("d", 64) },
		"restore_policy": func(v *store.NeutralChallenge) { v.RestorePolicy = "different" },
		"recovery_plan":  func(v *store.NeutralChallenge) { v.RecoveryPlanID = receiptTestPlan },
		"issued_at":      func(v *store.NeutralChallenge) { v.IssuedAt = v.IssuedAt.Add(time.Second) },
		"expires_at":     func(v *store.NeutralChallenge) { v.ExpiresAt = v.ExpiresAt.Add(-time.Second) },
	}
	for name, modify := range modifications {
		t.Run(name, func(t *testing.T) {
			altered := c
			modify(&altered)
			if err := verifier.VerifyNeutralReceipt(context.Background(), altered, raw); err == nil {
				t.Fatal("signature accepted for a different persisted challenge")
			}
		})
	}
}

func TestRecoveryReceiptCanBindZeroGenerationAndPlan(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	c := challengeFixture()
	c.Purpose, c.LeaseID, c.RecoveryPlanID, c.Generation, c.AgentHighWater = "recovery", "", receiptTestPlan, 0, 0
	observer := &fakeObserver{observation: observationFixture(c)}
	producer, err := NewProducer("board-agent-1", private, observer, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
	if err != nil {
		t.Fatal(err)
	}
	if err := testVerifier(t, public).VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
		t.Fatalf("plan-bound recovery receipt rejected: %v", err)
	}
}

func TestProducerRequiresRealObserverAndFailsClosedOnUnsafeState(t *testing.T) {
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	var typedNil *fakeObserver
	for _, observer := range []NeutralObserver{nil, typedNil} {
		if _, err := NewProducer("board-agent-1", private, observer, nil); !errors.Is(err, ErrObservationAbsent) {
			t.Fatalf("nil observer accepted: %v", err)
		}
	}
	if _, err := NewProducer("bad key", private, &fakeObserver{}, nil); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("invalid key ID accepted: %v", err)
	}
	c := challengeFixture()
	base := observationFixture(c)
	for name, mutate := range map[string]func(*Observation){
		"wrong_challenge":   func(v *Observation) { v.ChallengeID = receiptTestPlan },
		"wrong_nonce":       func(v *Observation) { v.Nonce = strings.Repeat("c", 64) },
		"not_neutral":       func(v *Observation) { v.Neutral = false },
		"no_evidence":       func(v *Observation) { v.Evidence = nil },
		"oversize_evidence": func(v *Observation) { v.Evidence = bytes.Repeat([]byte{'x'}, MaxEvidenceBytes+1) },
		"wrong_board":       func(v *Observation) { v.BoardID = "other" },
		"wrong_lease":       func(v *Observation) { v.LeaseID = receiptTestPlan },
		"wrong_generation":  func(v *Observation) { v.Generation++ },
		"wrong_high_water":  func(v *Observation) { v.AgentHighWater++ },
		"wrong_fixture":     func(v *Observation) { v.FixtureRevision = "other" },
		"wrong_profile":     func(v *Observation) { v.ProfileSHA256 = strings.Repeat("c", 64) },
		"before_issue":      func(v *Observation) { v.ObservedAt = c.IssuedAt.Add(-time.Second) },
		"future":            func(v *Observation) { v.ObservedAt = testNow.Add(time.Second) },
	} {
		t.Run(name, func(t *testing.T) {
			observation := base
			mutate(&observation)
			producer, err := NewProducer("board-agent-1", private,
				&fakeObserver{observation: observation}, func() time.Time { return testNow })
			if err != nil {
				t.Fatal(err)
			}
			if _, err := producer.ProduceNeutralReceipt(context.Background(), c); !errors.Is(err, ErrObservationAbsent) {
				t.Fatalf("unsafe physical state signed: %v", err)
			}
		})
	}
	observer := &fakeObserver{err: errors.New("sensor failed")}
	producer, err := NewProducer("board-agent-1", private, observer, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	if _, err := producer.ProduceNeutralReceipt(context.Background(), c); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("sensor error ignored: %v", err)
	}
	if _, err := (*Producer)(nil).ProduceNeutralReceipt(context.Background(), c); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("nil producer signed: %v", err)
	}
}

func TestChallengeClockAndShapeAreEnforced(t *testing.T) {
	c, _, public, private, raw := receiptFixture(t)
	verifier := testVerifier(t, public)
	for name, altered := range map[string]store.NeutralChallenge{
		"expired":               func() store.NeutralChallenge { v := c; v.ExpiresAt = testNow; return v }(),
		"not_issued":            func() store.NeutralChallenge { v := c; v.IssuedAt = testNow.Add(time.Second); return v }(),
		"too_long":              func() store.NeutralChallenge { v := c; v.ExpiresAt = v.IssuedAt.Add(time.Minute); return v }(),
		"bad_nonce":             func() store.NeutralChallenge { v := c; v.Nonce = "x"; return v }(),
		"bad_profile":           func() store.NeutralChallenge { v := c; v.ProfileSHA256 = "x"; return v }(),
		"missing_release_lease": func() store.NeutralChallenge { v := c; v.LeaseID = ""; return v }(),
	} {
		t.Run(name, func(t *testing.T) {
			if err := verifier.VerifyNeutralReceipt(context.Background(), altered, raw); err == nil {
				t.Fatal("invalid persisted challenge accepted")
			}
			producer, err := NewProducer("board-agent-1", private,
				&fakeObserver{observation: observationFixture(altered)}, func() time.Time { return testNow })
			if err != nil {
				t.Fatal(err)
			}
			if _, err := producer.ProduceNeutralReceipt(context.Background(), altered); err == nil {
				t.Fatal("producer signed invalid challenge")
			}
		})
	}
}

func TestVerifierRejectsUnknownKeysTamperingAndNoncanonicalWire(t *testing.T) {
	c, _, public, private, raw := receiptFixture(t)
	verifier := testVerifier(t, public)
	var receipt Receipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*Receipt){
		"signature":          func(v *Receipt) { v.Signature[0] ^= 0x40 },
		"unknown_key":        func(v *Receipt) { v.Payload.KeyID = "unlisted" },
		"bad_evidence_hash":  func(v *Receipt) { v.Payload.EvidenceSHA256 = "x" },
		"future_signed_time": func(v *Receipt) { v.Payload.SignedAt = formatTime(testNow.Add(time.Second)) },
		"before_observation": func(v *Receipt) { v.Payload.SignedAt = formatTime(testNow.Add(-2 * time.Second)) },
		"unknown_version":    func(v *Receipt) { v.Payload.Version = 2 },
		"unsafe_state":       func(v *Receipt) { v.Payload.State = "unknown" },
	} {
		t.Run(name, func(t *testing.T) {
			altered := receipt
			altered.Signature = append([]byte(nil), receipt.Signature...)
			mutate(&altered)
			encoded, err := json.Marshal(altered)
			if err != nil {
				t.Fatal(err)
			}
			if err := verifier.VerifyNeutralReceipt(context.Background(), c, encoded); err == nil {
				t.Fatal("altered receipt accepted")
			}
		})
	}
	for _, malformed := range [][]byte{nil, []byte("true"), append([]byte(nil), append(raw, []byte(" {}")...)...),
		append([]byte(" "), raw...), append(append([]byte(nil), raw...), '\n'), bytes.Repeat([]byte{'x'}, MaxReceiptBytes+1)} {
		if err := verifier.VerifyNeutralReceipt(context.Background(), c, malformed); err == nil {
			t.Fatal("noncanonical receipt accepted")
		}
	}
	if _, err := NewVerifier(nil, nil); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("empty key allowlist accepted: %v", err)
	}
	if _, err := NewVerifier(map[string]ed25519.PublicKey{"bad key": public}, nil); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("invalid key ID accepted: %v", err)
	}
	// The verifier must not observe later mutations to a caller-owned key map.
	keys := map[string]ed25519.PublicKey{"board-agent-1": append(ed25519.PublicKey(nil), public...)}
	copyVerifier, err := NewVerifier(keys, func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	keys["board-agent-1"][0] ^= 1
	delete(keys, "board-agent-1")
	if err := copyVerifier.VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
		t.Fatalf("key allowlist was mutated externally: %v", err)
	}
	_ = private
}

func TestVerifierRejectsCryptographicallyValidUnsafeClaims(t *testing.T) {
	c, _, public, private, raw := receiptFixture(t)
	verifier := testVerifier(t, public)
	var original Receipt
	if err := json.Unmarshal(raw, &original); err != nil {
		t.Fatal(err)
	}
	for name, mutate := range map[string]func(*Payload){
		"not_neutral":      func(p *Payload) { p.State = "unsafe" },
		"future_signed":    func(p *Payload) { p.SignedAt = formatTime(testNow.Add(time.Second)) },
		"before_observed":  func(p *Payload) { p.ObservedAt = formatTime(c.IssuedAt.Add(-time.Second)) },
		"invalid_evidence": func(p *Payload) { p.EvidenceSHA256 = "x" },
	} {
		t.Run(name, func(t *testing.T) {
			altered := original
			mutate(&altered.Payload)
			signed, err := signedBytes(altered.Payload)
			if err != nil {
				t.Fatal(err)
			}
			altered.Signature = ed25519.Sign(private, signed)
			encoded, err := json.Marshal(altered)
			if err != nil {
				t.Fatal(err)
			}
			if err := verifier.VerifyNeutralReceipt(context.Background(), c, encoded); !errors.Is(err, ErrInvalidReceipt) {
				t.Fatalf("signed unsafe claim accepted: %v", err)
			}
		})
	}
}
