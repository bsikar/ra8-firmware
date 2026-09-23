// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func backupGateFixture(t *testing.T) (*SignedBackupGate, BackupAttestation, ed25519.PrivateKey) {
	t.Helper()
	public, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Date(2026, 9, 23, 4, 0, 0, 0, time.UTC)
	gate, err := NewSignedBackupGate(filepath.Join(t.TempDir(), "backup.json"), public,
		"018d1234-5678-7abc-8def-123456789abc", 20*time.Minute, 48*time.Hour, 90*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	gate.now = func() time.Time { return now }
	return gate, BackupAttestation{SchemaVersion: backupAttestationSchema, ApprovalID: gate.approvalID,
		CheckedAt: now.Add(-time.Minute), LatestFullBackup: now.Add(-time.Hour),
		RestoreDrillAt: now.Add(-24 * time.Hour)}, private
}

func writeSignedBackupFixture(t *testing.T, path string, attestation BackupAttestation, key ed25519.PrivateKey) {
	t.Helper()
	raw, err := SignBackupAttestation(attestation, key)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, raw, 0o640); err != nil {
		t.Fatal(err)
	}
}

func TestSignedBackupGateAcceptsFreshTrustedEvidence(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("Check rejected valid evidence: %v", err)
	}
}

func TestSignedBackupGateRejectsTamperingAndWrongApproval(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	if err := gate.Check(context.Background(), "018d1234-5678-7abc-8def-123456789abd"); err == nil {
		t.Fatal("wrong configured approval was accepted")
	}
	raw, err := os.ReadFile(gate.path)
	if err != nil {
		t.Fatal(err)
	}
	var envelope map[string]any
	if err := json.Unmarshal(raw, &envelope); err != nil {
		t.Fatal(err)
	}
	envelope["approval_id"] = "018d1234-5678-7abc-8def-123456789abd"
	raw, _ = json.Marshal(envelope)
	if err := os.WriteFile(gate.path, raw, 0o640); err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), gate.approvalID); err == nil {
		t.Fatal("modified signed evidence was accepted")
	}
}

func TestSignedBackupGateRejectsStaleEvidenceAndWritableFiles(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-time.Hour)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	if err := gate.Check(context.Background(), gate.approvalID); err == nil {
		t.Fatal("stale backup check was accepted")
	}
	attestation.CheckedAt = gate.now().Add(-time.Minute)
	writeSignedBackupFixture(t, gate.path, attestation, key)
	if err := os.Chmod(gate.path, 0o666); err != nil {
		t.Fatal(err)
	}
	if err := gate.Check(context.Background(), gate.approvalID); err == nil {
		t.Fatal("writable attestation was accepted")
	}
}
