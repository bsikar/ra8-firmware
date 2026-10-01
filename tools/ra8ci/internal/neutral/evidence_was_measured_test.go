package neutral

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// resignedWith rebuilds a receipt around a mutated payload so the only thing
// under test is the rule, not a broken signature.
func resignedWith(t *testing.T, private ed25519.PrivateKey, payload Payload) []byte {
	t.Helper()
	toSign, err := signedBytes(payload)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := json.Marshal(Receipt{Payload: payload, Signature: ed25519.Sign(private, toSign)})
	if err != nil {
		t.Fatal(err)
	}
	return raw
}

func payloadOf(t *testing.T, raw []byte) Payload {
	t.Helper()
	var receipt Receipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		t.Fatal(err)
	}
	return receipt.Payload
}

// The constant the rule refuses is transcribed here rather than read from the
// file under test, and then cross-checked against the hash itself.
func TestDigestOfNothingIsTheHashOfNoBytes(t *testing.T) {
	const transcribed = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
	if digestOfNothing != transcribed {
		t.Fatalf("constant drifted: %q", digestOfNothing)
	}
	for name, input := range map[string][]byte{"nil": nil, "empty_slice": {}} {
		sum := sha256.Sum256(input)
		if got := hex.EncodeToString(sum[:]); got != transcribed {
			t.Fatalf("%s hashed to %q", name, got)
		}
	}
	if !validSHA256(digestOfNothing) {
		t.Fatal("the empty digest is not even well-formed, so the shape rule alone would have caught it")
	}
}

func TestEvidenceWasMeasuredJudgesDigestShapeAndEmptiness(t *testing.T) {
	sumOfSomething := sha256.Sum256([]byte("power isolated; SWD idle"))
	measured := hex.EncodeToString(sumOfSomething[:])
	oneByte := sha256.Sum256([]byte{0})
	cases := map[string]struct {
		digest string
		want   bool
	}{
		"real_evidence":    {measured, true},
		"one_byte":         {hex.EncodeToString(oneByte[:]), true},
		"empty":            {digestOfNothing, false},
		"empty_uppercased": {strings.ToUpper(digestOfNothing), false},
		"blank":            {"", false},
		"not_hex":          {strings.Repeat("z", 64), false},
		"short":            {strings.Repeat("a", 63), false},
		"long":             {strings.Repeat("a", 65), false},
		"all_zeroes":       {strings.Repeat("0", 64), true},
	}
	for name, testCase := range cases {
		t.Run(name, func(t *testing.T) {
			if got := evidenceWasMeasured(testCase.digest); got != testCase.want {
				t.Fatalf("evidenceWasMeasured(%q) = %v, want %v", testCase.digest, got, testCase.want)
			}
		})
	}
}

// The only difference between the two receipts here is the evidence digest, so
// nothing but the new rule can explain the second one being refused.
func TestVerifierRefusesAReceiptStatingTheEmptyEvidenceDigest(t *testing.T) {
	c, _, public, private, raw := receiptFixture(t)
	verifier := testVerifier(t, public)
	if err := verifier.VerifyNeutralReceipt(context.Background(), c, raw); err != nil {
		t.Fatalf("the measured receipt was rejected before the rule was reached: %v", err)
	}
	payload := payloadOf(t, raw)
	if payload.EvidenceSHA256 == digestOfNothing {
		t.Fatal("fixture evidence hashed to the empty digest")
	}
	measured := resignedWith(t, private, payload)
	if err := verifier.VerifyNeutralReceipt(context.Background(), c, measured); err != nil {
		t.Fatalf("re-signing the untouched payload changed the answer: %v", err)
	}
	payload.EvidenceSHA256 = digestOfNothing
	if err := verifier.VerifyNeutralReceipt(context.Background(), c, resignedWith(t, private, payload)); !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("a receipt whose evidence was nothing verified: %v", err)
	}
}

// An unmeasured digest is refused whichever purpose the challenge was issued
// for, and refused by its own rule rather than by the key allowlist.
func TestEmptyEvidenceIsRefusedForEveryChallengePurpose(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	verifier, err := NewVerifier(map[string]ed25519.PublicKey{"board-agent-1": public},
		func() time.Time { return testNow })
	if err != nil {
		t.Fatal(err)
	}
	for _, purpose := range []string{"release", "recovery"} {
		t.Run(purpose, func(t *testing.T) {
			c := challengeFixture()
			if purpose == "recovery" {
				c.Purpose, c.LeaseID, c.RecoveryPlanID, c.Generation, c.AgentHighWater = "recovery", "", receiptTestPlan, 0, 0
			}
			producer, err := NewProducer("board-agent-1", private, &fakeObserver{observation: observationFixture(c)},
				func() time.Time { return testNow })
			if err != nil {
				t.Fatal(err)
			}
			raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
			if err != nil {
				t.Fatal(err)
			}
			payload := payloadOf(t, raw)
			payload.EvidenceSHA256 = digestOfNothing
			if err := verifier.VerifyNeutralReceipt(context.Background(), c, resignedWith(t, private, payload)); !errors.Is(err, ErrInvalidReceipt) {
				t.Fatalf("%s receipt with no evidence verified: %v", purpose, err)
			}
		})
	}
}

// The producer door the verifier is backing up: no producer here would ever
// sign the empty digest, because it refuses the observation that would make it.
func TestProducerNeverSignsAnEmptyEvidenceDigest(t *testing.T) {
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	c := challengeFixture()
	for name, evidence := range map[string][]byte{"nil": nil, "empty_slice": {}} {
		t.Run(name, func(t *testing.T) {
			observation := observationFixture(c)
			observation.Evidence = evidence
			producer, err := NewProducer("board-agent-1", private,
				&fakeObserver{observation: observation}, func() time.Time { return testNow })
			if err != nil {
				t.Fatal(err)
			}
			if _, err := producer.ProduceNeutralReceipt(context.Background(), c); !errors.Is(err, ErrObservationAbsent) {
				t.Fatalf("an observation with no evidence was signed: %v", err)
			}
		})
	}
}

// Every receipt this package produces states a digest the rule accepts, so the
// rule refuses nothing a working board agent sends.
func TestProducedReceiptsStateMeasuredEvidence(t *testing.T) {
	_, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	c := challengeFixture()
	for _, evidence := range [][]byte{[]byte("x"), []byte("power isolated; SWD idle"),
		[]byte(strings.Repeat("e", MaxEvidenceBytes))} {
		observation := observationFixture(c)
		observation.Evidence = evidence
		producer, err := NewProducer("board-agent-1", private,
			&fakeObserver{observation: observation}, func() time.Time { return testNow })
		if err != nil {
			t.Fatal(err)
		}
		raw, err := producer.ProduceNeutralReceipt(context.Background(), c)
		if err != nil {
			t.Fatalf("%d-byte evidence refused by the producer: %v", len(evidence), err)
		}
		digest := payloadOf(t, raw).EvidenceSHA256
		if !evidenceWasMeasured(digest) {
			t.Fatalf("%d-byte evidence produced a digest the verifier refuses: %q", len(evidence), digest)
		}
	}
}

// The rule is the verifier's own, not the store's: an unmeasured digest is
// refused before any one-use challenge is spent.
func TestEmptyEvidenceIsRefusedWithoutConsultingTheChallengeStore(t *testing.T) {
	c, _, public, private, raw := receiptFixture(t)
	payload := payloadOf(t, raw)
	payload.EvidenceSHA256 = digestOfNothing
	empty := resignedWith(t, private, payload)
	var verifier store.NeutralReceiptVerifier = testVerifier(t, public)
	if err := verifier.VerifyNeutralReceipt(context.Background(), c, empty); !errors.Is(err, ErrInvalidReceipt) {
		t.Fatalf("store-facing verifier accepted an unmeasured receipt: %v", err)
	}
}
