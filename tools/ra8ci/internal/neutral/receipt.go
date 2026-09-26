// Package neutral signs and verifies board-agent observations bound to a
// database-issued, one-use neutral challenge. It does not observe hardware;
// production must inject a privileged physical observer on the board agent.
package neutral

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"reflect"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	Version           = 1
	MaxReceiptBytes   = 65536
	MaxEvidenceBytes  = 16384
	maxChallengeAge   = 30 * time.Second
	maxObservationAge = 5 * time.Second
)

const domain = "ra8ci/board-neutral-receipt/v1\x00"

var (
	ErrInvalidChallenge  = errors.New("invalid neutral challenge")
	ErrObservationAbsent = errors.New("physical neutral observation is absent or unsafe")
	ErrInvalidReceipt    = errors.New("invalid neutral receipt")
	ErrUnknownKey        = errors.New("neutral receipt signing key is not allowlisted")
	ErrExpired           = errors.New("neutral challenge is not live")
)

// Observation is returned only by the injected physical observer. Its
// Neutral value is not taken from an HTTP request or from a caller assertion.
// Evidence is fixture-specific sensor/probe evidence; this codec signs its
// digest, while the hardware adapter owns its semantic interpretation.
type Observation struct {
	ChallengeID     string
	Nonce           string
	BoardID         string
	LeaseID         string
	Generation      uint64
	AgentHighWater  uint64
	FixtureRevision string
	ProfileSHA256   string
	Neutral         bool
	Evidence        []byte
	ObservedAt      time.Time
}

// NeutralObserver must independently inspect the physical fixture under the
// board agent's serialized hardware gate. No default or shell-based observer
// is provided. A nil observer cannot sign a receipt.
type NeutralObserver interface {
	ObserveNeutral(context.Context, store.NeutralChallenge) (Observation, error)
}

type Payload struct {
	Version         int    `json:"version"`
	KeyID           string `json:"key_id"`
	ChallengeID     string `json:"challenge_id"`
	Nonce           string `json:"nonce"`
	BoardID         string `json:"board_id"`
	Purpose         string `json:"purpose"`
	LeaseID         string `json:"lease_id"`
	Generation      uint64 `json:"generation"`
	SnapshotVersion uint64 `json:"snapshot_version"`
	AgentHighWater  uint64 `json:"agent_high_water"`
	FixtureRevision string `json:"fixture_revision"`
	ProfileSHA256   string `json:"profile_sha256"`
	RestorePolicy   string `json:"restore_policy"`
	RecoveryPlanID  string `json:"recovery_plan_id"`
	IssuedAt        string `json:"issued_at"`
	ExpiresAt       string `json:"expires_at"`
	State           string `json:"state"`
	ObservedAt      string `json:"observed_at"`
	EvidenceSHA256  string `json:"evidence_sha256"`
	SignedAt        string `json:"signed_at"`
}

type Receipt struct {
	Payload   Payload `json:"payload"`
	Signature []byte  `json:"signature"`
}

// Producer signs only after its physical observer returns explicit neutral
// state for the same board, fixture profile, lease and agent high-water.
type Producer struct {
	keyID    string
	private  ed25519.PrivateKey
	observer NeutralObserver
	now      func() time.Time
}

// NewProducer copies the private key. It refuses any absent physical observer.
func NewProducer(keyID string, private ed25519.PrivateKey, observer NeutralObserver, now func() time.Time) (*Producer, error) {
	if !validKeyID(keyID) || len(private) != ed25519.PrivateKeySize || nilObserver(observer) {
		return nil, ErrObservationAbsent
	}
	if now == nil {
		now = time.Now
	}
	return &Producer{keyID: keyID, private: append(ed25519.PrivateKey(nil), private...),
		observer: observer, now: now}, nil
}

func nilObserver(observer NeutralObserver) bool {
	if observer == nil {
		return true
	}
	value := reflect.ValueOf(observer)
	switch value.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Pointer, reflect.Slice:
		return value.IsNil()
	default:
		return false
	}
}

// ProduceNeutralReceipt satisfies boardclient.NeutralReceiptProducer. The
// verifier still requires a matching allowlisted key and the PostgreSQL store
// atomically consumes the one-use challenge with the board transition.
func (p *Producer) ProduceNeutralReceipt(ctx context.Context, challenge store.NeutralChallenge) ([]byte, error) {
	if p == nil || nilObserver(p.observer) || len(p.private) != ed25519.PrivateKeySize || !validKeyID(p.keyID) {
		return nil, ErrObservationAbsent
	}
	now := p.now().UTC()
	if err := validateChallenge(challenge, now); err != nil {
		return nil, err
	}
	observation, err := p.observer.ObserveNeutral(ctx, challenge)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrObservationAbsent, err)
	}
	now = p.now().UTC()
	if err := validateChallenge(challenge, now); err != nil {
		return nil, err
	}
	if !validObservation(challenge, observation, now) {
		return nil, ErrObservationAbsent
	}
	evidenceHash := sha256.Sum256(observation.Evidence)
	payload := payloadFor(challenge, p.keyID, observation.ObservedAt.UTC(), now, hex.EncodeToString(evidenceHash[:]))
	toSign, err := signedBytes(payload)
	if err != nil {
		return nil, err
	}
	receipt := Receipt{Payload: payload, Signature: ed25519.Sign(p.private, toSign)}
	encoded, err := json.Marshal(receipt)
	if err != nil || len(encoded) > MaxReceiptBytes {
		return nil, ErrInvalidReceipt
	}
	return encoded, nil
}

// Verifier holds an immutable key allowlist and checks the exact persisted
// challenge. The database, not this in-memory object, enforces one-use CAS.
type Verifier struct {
	keys map[string]ed25519.PublicKey
	now  func() time.Time
}

func NewVerifier(keys map[string]ed25519.PublicKey, now func() time.Time) (*Verifier, error) {
	if len(keys) == 0 {
		return nil, ErrUnknownKey
	}
	copyKeys := make(map[string]ed25519.PublicKey, len(keys))
	for id, key := range keys {
		if !validKeyID(id) || len(key) != ed25519.PublicKeySize {
			return nil, ErrUnknownKey
		}
		copyKeys[id] = append(ed25519.PublicKey(nil), key...)
	}
	if now == nil {
		now = time.Now
	}
	return &Verifier{keys: copyKeys, now: now}, nil
}

// VerifyNeutralReceipt implements store.NeutralReceiptVerifier. It never
// treats a nonempty receipt or a boolean as proof.
func (v *Verifier) VerifyNeutralReceipt(_ context.Context, challenge store.NeutralChallenge, raw []byte) error {
	if v == nil || len(v.keys) == 0 || len(raw) == 0 || len(raw) > MaxReceiptBytes {
		return ErrInvalidReceipt
	}
	now := v.now().UTC()
	if err := validateChallenge(challenge, now); err != nil {
		return err
	}
	var receipt Receipt
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&receipt); err != nil {
		return ErrInvalidReceipt
	}
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		return ErrInvalidReceipt
	}
	canonical, err := json.Marshal(receipt)
	if err != nil || !bytes.Equal(canonical, raw) {
		return ErrInvalidReceipt
	}
	public, allowed := v.keys[receipt.Payload.KeyID]
	if !allowed {
		return ErrUnknownKey
	}
	if len(receipt.Signature) != ed25519.SignatureSize || !payloadMatchesChallenge(receipt.Payload, challenge) {
		return ErrInvalidReceipt
	}
	observedAt, err := time.Parse(time.RFC3339Nano, receipt.Payload.ObservedAt)
	if err != nil || formatTime(observedAt) != receipt.Payload.ObservedAt {
		return ErrInvalidReceipt
	}
	signedAt, err := time.Parse(time.RFC3339Nano, receipt.Payload.SignedAt)
	if err != nil || formatTime(signedAt) != receipt.Payload.SignedAt || receipt.Payload.State != "neutral" ||
		observedAt.Before(challenge.IssuedAt) || !checkObservationIsFresh(observedAt, signedAt) ||
		signedAt.After(now) || !signedAt.Before(challenge.ExpiresAt) ||
		!validSHA256(receipt.Payload.EvidenceSHA256) {
		return ErrInvalidReceipt
	}
	toVerify, err := signedBytes(receipt.Payload)
	if err != nil || !ed25519.Verify(public, toVerify, receipt.Signature) {
		return ErrInvalidReceipt
	}
	return nil
}

var _ store.NeutralReceiptVerifier = (*Verifier)(nil)

func signedBytes(payload Payload) ([]byte, error) {
	encoded, err := json.Marshal(payload)
	if err != nil {
		return nil, ErrInvalidReceipt
	}
	return append([]byte(domain), encoded...), nil
}

func payloadFor(c store.NeutralChallenge, keyID string, observedAt, signedAt time.Time, evidenceHash string) Payload {
	return Payload{Version: Version, KeyID: keyID, ChallengeID: c.ID, Nonce: c.Nonce,
		BoardID: c.BoardID, Purpose: c.Purpose, LeaseID: c.LeaseID,
		Generation: c.Generation, SnapshotVersion: c.SnapshotVersion,
		AgentHighWater: c.AgentHighWater, FixtureRevision: c.FixtureRevision,
		ProfileSHA256: c.ProfileSHA256, RestorePolicy: c.RestorePolicy,
		RecoveryPlanID: c.RecoveryPlanID, IssuedAt: formatTime(c.IssuedAt),
		ExpiresAt: formatTime(c.ExpiresAt), State: "neutral", ObservedAt: formatTime(observedAt),
		EvidenceSHA256: evidenceHash, SignedAt: formatTime(signedAt)}
}

func payloadMatchesChallenge(p Payload, c store.NeutralChallenge) bool {
	expected := payloadFor(c, p.KeyID, time.Time{}, time.Time{}, "")
	return p.Version == Version && p.KeyID != "" && p.ChallengeID == expected.ChallengeID &&
		p.Nonce == expected.Nonce && p.BoardID == expected.BoardID && p.Purpose == expected.Purpose &&
		p.LeaseID == expected.LeaseID && p.Generation == expected.Generation &&
		p.SnapshotVersion == expected.SnapshotVersion && p.AgentHighWater == expected.AgentHighWater &&
		p.FixtureRevision == expected.FixtureRevision && p.ProfileSHA256 == expected.ProfileSHA256 &&
		p.RestorePolicy == expected.RestorePolicy && p.RecoveryPlanID == expected.RecoveryPlanID &&
		p.IssuedAt == expected.IssuedAt && p.ExpiresAt == expected.ExpiresAt
}

func validObservation(c store.NeutralChallenge, o Observation, now time.Time) bool {
	return o.Neutral && o.ChallengeID == c.ID && o.Nonce == c.Nonce &&
		o.BoardID == c.BoardID && o.LeaseID == c.LeaseID &&
		o.Generation == c.Generation && o.AgentHighWater == c.AgentHighWater &&
		o.FixtureRevision == c.FixtureRevision && o.ProfileSHA256 == c.ProfileSHA256 &&
		len(o.Evidence) > 0 && len(o.Evidence) <= MaxEvidenceBytes &&
		!o.ObservedAt.IsZero() && !o.ObservedAt.Before(c.IssuedAt) &&
		!o.ObservedAt.After(now) && now.Sub(o.ObservedAt) <= maxObservationAge &&
		o.ObservedAt.Before(c.ExpiresAt)
}

func validateChallenge(c store.NeutralChallenge, now time.Time) error {
	if !store.ValidID(c.ID) || !validHex(c.Nonce, 64) || !validBoardID(c.BoardID) ||
		(c.Purpose != "release" && c.Purpose != "recovery") ||
		(c.LeaseID != "" && !store.ValidID(c.LeaseID)) ||
		(c.Purpose == "release" && c.LeaseID == "") ||
		(c.Purpose == "release" && (c.RecoveryPlanID != "" || c.AgentHighWater != c.Generation)) ||
		(c.Purpose == "recovery" && !store.ValidID(c.RecoveryPlanID)) ||
		(c.Purpose == "release" && c.Generation == 0) || c.FixtureRevision == "" || !validSHA256(c.ProfileSHA256) ||
		c.RestorePolicy == "" || c.IssuedAt.IsZero() || c.ExpiresAt.IsZero() ||
		!c.ExpiresAt.After(c.IssuedAt) || c.ExpiresAt.Sub(c.IssuedAt) > maxChallengeAge {
		return ErrInvalidChallenge
	}
	if now.Before(c.IssuedAt) || !now.Before(c.ExpiresAt) {
		return ErrExpired
	}
	return nil
}

func validKeyID(id string) bool {
	if id == "" || len(id) > 64 {
		return false
	}
	for _, r := range id {
		if !((r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' || r == '.') {
			return false
		}
	}
	return true
}

func validBoardID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if !((r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' || r == '.') {
			return false
		}
	}
	return true
}

func validSHA256(value string) bool { return validHex(value, 64) }

func validHex(value string, length int) bool {
	if len(value) != length || strings.ToLower(value) != value {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func formatTime(at time.Time) string { return at.UTC().Format(time.RFC3339Nano) }
