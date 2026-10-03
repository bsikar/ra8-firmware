// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"os"
	"strings"
	"testing"
	"time"
)

// The gate ages three timestamps against its own clock, each with its own
// window and its own message. A signature that verifies says the monitor
// wrote the envelope; it says nothing about whether the evidence inside is
// still worth reserving capacity on.

// A full backup older than the backup window is refused on its own terms,
// even though the check that observed it is fresh and correctly signed.
func TestAFullBackupOlderThanItsWindowIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.LatestFullBackup = gate.now().Add(-49 * time.Hour)
	attestation.RestoreDrillAt = gate.now().Add(-49 * time.Hour)
	writeSignedBackupFixture(t, gate.path, attestation, key)

	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil || !strings.HasPrefix(err.Error(), "full backup evidence:") {
		t.Fatalf("answered %v, want the full backup named as stale", err)
	}
}

// A restore drill older than the drill window is refused the same way, and
// this one is reached only because the backup ahead of it was inside its own
// window, so the case also pins that each timestamp is aged separately rather
// than the oldest of them standing for all three.
func TestARestoreDrillOlderThanItsWindowIsRefused(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.RestoreDrillAt = gate.now().Add(-91 * 24 * time.Hour)
	writeSignedBackupFixture(t, gate.path, attestation, key)

	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil || !strings.HasPrefix(err.Error(), "restore drill evidence:") {
		t.Fatalf("answered %v, want the restore drill named as stale", err)
	}
}

// Each window is judged at its edge, so evidence just inside it is still
// accepted. Without this the staleness cases above would also pass against a
// gate that refused everything.
func TestEvidenceJustInsideEveryWindowIsStillAccepted(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = gate.now().Add(-20 * time.Minute)
	attestation.LatestFullBackup = gate.now().Add(-48 * time.Hour)
	attestation.RestoreDrillAt = gate.now().Add(-90 * 24 * time.Hour)
	writeSignedBackupFixture(t, gate.path, attestation, key)

	if err := gate.Check(context.Background(), gate.approvalID); err != nil {
		t.Fatalf("evidence at the edge of every window was refused: %v", err)
	}
}



// The monitor signs nothing it could not have observed, so the same coherence
// rule the gate applies on the way in is applied on the way out. A signing
// request that carries no check time is refused before a signature exists,
// which is what keeps an incoherent envelope from ever being written.
func TestSigningRefusesARequestWithNoCheckTime(t *testing.T) {
	_, attestation, key := backupGateFixture(t)
	attestation.CheckedAt = time.Time{}

	if _, err := SignBackupAttestation(attestation, key); err == nil ||
		!strings.Contains(err.Error(), "invalid backup attestation signing request") {
		t.Fatalf("answered %v, want the signing request refused", err)
	}
}

// Evidence the monitor never gathered is refused by name, so the operator is
// told which half of the envelope is empty rather than that it is invalid.
func TestSigningRefusesEvidenceThatIsSimplyAbsent(t *testing.T) {
	_, valid, key := backupGateFixture(t)

	for name, absent := range map[string]func(a *BackupAttestation){
		"no full backup":   func(a *BackupAttestation) { a.LatestFullBackup = time.Time{} },
		"no restore drill": func(a *BackupAttestation) { a.RestoreDrillAt = time.Time{} },
	} {
		t.Run(name, func(t *testing.T) {
			attestation := valid
			absent(&attestation)

			if _, err := SignBackupAttestation(attestation, key); err == nil {
				t.Fatal("absent evidence was signed")
			}
		})
	}
}

// A hand-built envelope that claims a check but carries no backup time reaches
// the gate's coherence rule rather than its clock, so the refusal names the
// missing evidence instead of aging a zero timestamp.
func TestTheGateNamesAbsentEvidenceRatherThanAgingIt(t *testing.T) {
	gate, attestation, key := backupGateFixture(t)
	attestation.LatestFullBackup = time.Time{}
	if err := os.WriteFile(gate.path, signRawAttestation(t, attestation, key), 0o640); err != nil {
		t.Fatal(err)
	}

	err := gate.Check(context.Background(), gate.approvalID)
	if err == nil || !strings.Contains(err.Error(), "full backup evidence is absent") {
		t.Fatalf("answered %v, want the absent full backup named", err)
	}
}
