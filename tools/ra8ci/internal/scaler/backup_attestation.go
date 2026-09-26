// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const backupAttestationSchema = 1
const maxBackupAttestationBytes = 16 << 10

// BackupAttestation is signed by a separate, root-owned backup monitor. Its
// signing key must never be readable by the ra8ci service account.
type BackupAttestation struct {
	SchemaVersion    int       `json:"schema_version"`
	ApprovalID       string    `json:"approval_id"`
	CheckedAt        time.Time `json:"checked_at"`
	LatestFullBackup time.Time `json:"latest_full_backup_at"`
	RestoreDrillAt   time.Time `json:"restore_drill_at"`
	Signature        string    `json:"signature"`
}

type backupAttestationPayload struct {
	SchemaVersion    int       `json:"schema_version"`
	ApprovalID       string    `json:"approval_id"`
	CheckedAt        time.Time `json:"checked_at"`
	LatestFullBackup time.Time `json:"latest_full_backup_at"`
	RestoreDrillAt   time.Time `json:"restore_drill_at"`
}

// SignBackupAttestation produces the exact envelope consumed by
// SignedBackupGate. Call this only from the separately privileged backup
// monitor after it has independently checked pgBackRest and restore evidence.
func SignBackupAttestation(attestation BackupAttestation, key ed25519.PrivateKey) ([]byte, error) {
	if len(key) != ed25519.PrivateKeySize || attestation.SchemaVersion != backupAttestationSchema ||
		!store.ValidID(attestation.ApprovalID) || attestation.CheckedAt.IsZero() ||
		attestation.LatestFullBackup.IsZero() || attestation.RestoreDrillAt.IsZero() || attestation.Signature != "" {
		return nil, errors.New("invalid backup attestation signing request")
	}
	if err := checkObservesItsEvidence(attestation); err != nil {
		return nil, fmt.Errorf("invalid backup attestation signing request: %w", err)
	}
	payload := payloadFromAttestation(attestation)
	encoded, err := json.Marshal(payload)
	if err != nil {
		return nil, errors.New("encode backup attestation payload")
	}
	attestation.Signature = base64.RawStdEncoding.EncodeToString(ed25519.Sign(key, encoded))
	result, err := json.Marshal(attestation)
	if err != nil {
		return nil, errors.New("encode signed backup attestation")
	}
	return append(result, '\n'), nil
}

// SignedBackupGate requires fresh cryptographic evidence from the privileged
// monitor before the scaler can reserve new runner capacity.
type SignedBackupGate struct {
	path         string
	publicKey    ed25519.PublicKey
	approvalID   string
	maxCheckAge  time.Duration
	maxBackupAge time.Duration
	maxDrillAge  time.Duration
	now          func() time.Time
}

// NewSignedBackupGate validates immutable policy and copies the public key.
func NewSignedBackupGate(path string, publicKey ed25519.PublicKey, approvalID string,
	maxCheckAge, maxBackupAge, maxDrillAge time.Duration) (*SignedBackupGate, error) {
	if !filepath.IsAbs(path) || filepath.Clean(path) != path || len(publicKey) != ed25519.PublicKeySize ||
		!store.ValidID(approvalID) || maxCheckAge <= 0 || maxCheckAge > time.Hour ||
		maxBackupAge <= 0 || maxBackupAge > 7*24*time.Hour ||
		maxDrillAge <= 0 || maxDrillAge > 365*24*time.Hour {
		return nil, errors.New("invalid signed backup gate configuration")
	}
	return &SignedBackupGate{path: path, publicKey: append(ed25519.PublicKey(nil), publicKey...),
		approvalID: approvalID, maxCheckAge: maxCheckAge, maxBackupAge: maxBackupAge,
		maxDrillAge: maxDrillAge, now: time.Now}, nil
}

// Check rejects absent, stale, modified, or policy-mismatched evidence. The
// verifier has no write path and never runs backup commands as the service.
func (g *SignedBackupGate) Check(ctx context.Context, approvalID string) error {
	if g == nil || ctx == nil || approvalID != g.approvalID || !store.ValidID(approvalID) {
		return errors.New("backup readiness approval does not match configured policy")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	info, err := os.Lstat(g.path)
	if err != nil {
		return fmt.Errorf("stat signed backup attestation: %w", err)
	}
	if !info.Mode().IsRegular() || info.Mode()&os.ModeSymlink != 0 || info.Size() <= 0 ||
		info.Size() > maxBackupAttestationBytes || info.Mode().Perm()&0022 != 0 {
		return errors.New("backup attestation must be a bounded, non-writable regular file")
	}
	file, err := os.Open(g.path)
	if err != nil {
		return errors.New("open signed backup attestation")
	}
	defer file.Close()
	opened, err := file.Stat()
	if err != nil || !opened.Mode().IsRegular() || !os.SameFile(info, opened) || opened.Size() > maxBackupAttestationBytes {
		return errors.New("backup attestation changed while opening")
	}
	raw, err := io.ReadAll(io.LimitReader(file, maxBackupAttestationBytes+1))
	if err != nil || len(raw) > maxBackupAttestationBytes {
		return errors.New("backup attestation exceeds size limit")
	}
	var attestation BackupAttestation
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&attestation); err != nil {
		return errors.New("decode signed backup attestation")
	}
	if err := expectBackupJSONEOF(decoder); err != nil {
		return err
	}
	if attestation.SchemaVersion != backupAttestationSchema || attestation.ApprovalID != g.approvalID {
		return errors.New("backup attestation schema or approval identity mismatch")
	}
	signature, err := base64.RawStdEncoding.DecodeString(attestation.Signature)
	if err != nil || len(signature) != ed25519.SignatureSize {
		return errors.New("backup attestation signature is malformed")
	}
	encoded, err := json.Marshal(payloadFromAttestation(attestation))
	if err != nil || !ed25519.Verify(g.publicKey, encoded, signature) {
		return errors.New("backup attestation signature verification failed")
	}
	// The envelope has to agree with itself before its dates are worth
	// measuring against this clock: a monitor cannot have observed a backup
	// or a drill that finished after the check it signed.
	if err := checkObservesItsEvidence(attestation); err != nil {
		return fmt.Errorf("backup attestation is internally inconsistent: %w", err)
	}
	now := g.now().UTC()
	if err := validAttestationTime(attestation.CheckedAt, now, g.maxCheckAge); err != nil {
		return fmt.Errorf("backup check evidence: %w", err)
	}
	if err := validAttestationTime(attestation.LatestFullBackup, now, g.maxBackupAge); err != nil {
		return fmt.Errorf("full backup evidence: %w", err)
	}
	if err := validAttestationTime(attestation.RestoreDrillAt, now, g.maxDrillAge); err != nil {
		return fmt.Errorf("restore drill evidence: %w", err)
	}
	return nil
}

func payloadFromAttestation(attestation BackupAttestation) backupAttestationPayload {
	return backupAttestationPayload{SchemaVersion: attestation.SchemaVersion, ApprovalID: attestation.ApprovalID,
		CheckedAt: attestation.CheckedAt, LatestFullBackup: attestation.LatestFullBackup,
		RestoreDrillAt: attestation.RestoreDrillAt}
}

func validAttestationTime(value, now time.Time, maxAge time.Duration) error {
	if value.IsZero() || value.After(now.Add(backupClockSkew)) || now.Sub(value) > maxAge {
		return errors.New("timestamp is absent, too old, or unexpectedly in the future")
	}
	return nil
}

func expectBackupJSONEOF(decoder *json.Decoder) error {
	var trailing any
	if err := decoder.Decode(&trailing); err != io.EOF {
		return errors.New("backup attestation has trailing JSON")
	}
	return nil
}
