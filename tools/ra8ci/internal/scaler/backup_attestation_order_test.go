// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"os"
	"strings"
	"testing"
	"time"
)

// signRawAttestation signs an envelope the way the monitor does but without
// SignBackupAttestation's refusals, so the gate can be shown refusing
// incoherent evidence on its own rather than inheriting the signer's check.
func signRawAttestation(t *testing.T, attestation BackupAttestation, key ed25519.PrivateKey) []byte {
	t.Helper()
	encoded, err := json.Marshal(payloadFromAttestation(attestation))
	if err != nil {
		t.Fatal(err)
	}
	attestation.Signature = base64.RawStdEncoding.EncodeToString(ed25519.Sign(key, encoded))
	raw, err := json.Marshal(attestation)
	if err != nil {
		t.Fatal(err)
	}
	return append(raw, '\n')
}

func TestSignedBackupGateRefusesABackupDatedAfterItsCheck(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-10 * time.Minute)
	attestation.LatestFullBackup = gate.now().Add(-time.Minute)
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, key), 0o640); err != nil {
		t.Fatal(err)
	}
	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil {
		t.Fatal("a full backup dated after the check that signed it was accepted")
	}
	if !strings.Contains(err.Error(), "after the check") {
		t.Fatalf("refusal does not name the relation that failed: %v", err)
	}
}

func TestSignedBackupGateRefusesADrillDatedAfterItsCheck(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-10 * time.Minute)
	attestation.RestoreDrillAt = gate.now().Add(-time.Minute)
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, key), 0o640); err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), gate.approvalID); err == nil {
		t.Fatal("a restore drill dated after the check that signed it was accepted")
	}
}

// The relation is the point, not a second freshness rule: an observation
// inside the skew allowance, and one taken at the check instant itself, are
// both ordinary and stay accepted.
func TestSignedBackupGateAcceptsAnObservationInsideTheSkewAllowance(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-10 * time.Minute)
	attestation.LatestFullBackup = attestation.CheckedAt.Add(time.Minute)
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, key), 0o640); err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("Check refused an observation within clock skew: %v", err)
	}
}

func TestSignedBackupGateAcceptsObservationsAtTheCheckInstant(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-10 * time.Minute)
	attestation.LatestFullBackup = attestation.CheckedAt
	attestation.RestoreDrillAt = attestation.CheckedAt
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, key), 0o640); err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("Check refused evidence observed at the check instant: %v", err)
	}
}

// A forged envelope still fails on the signature, so the new refusal has not
// become a way to learn anything about an unsigned attestation.
func TestSignedBackupGateStillRefusesAnUnsignedIncoherentAttestation(t *testing.T) {
	gate, attestation, _ := backupGateFixture(t)
	_, other, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	attestation.CheckedAt = gate.now().Add(-10 * time.Minute)
	attestation.LatestFullBackup = gate.now().Add(-time.Minute)
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, other), 0o640); err != nil {
		t.Fatal(err)
	}
	err = gate.Check(context.Background(), gate.approvalID)
	if err == nil {
		t.Fatal("an attestation signed by a foreign key was accepted")
	}
	if !strings.Contains(err.Error(), "signature verification failed") {
		t.Fatalf("want a signature refusal before any coherence reading, got: %v", err)
	}
}

func TestSignBackupAttestationRefusesEvidenceItCouldNotHaveObserved(t *testing.T) {
	_, attestation, key := backupGateFixture(t)
	attestation.RestoreDrillAt = attestation.CheckedAt.Add(time.Hour)
	if _, err := SignBackupAttestation(attestation, key); err == nil {
		t.Fatal("the monitor signed a drill dated after its own check")
	}
}

func TestSignBackupAttestationStillSignsOrderedEvidence(t *testing.T) {
	_, attestation, key := backupGateFixture(t)
	if _, err := SignBackupAttestation(attestation, key); err != nil {
		t.Fatalf("SignBackupAttestation refused ordered evidence: %v", err)
	}
}
