// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"strings"
	"testing"
	"time"
)

// checkObservesItsEvidence is the one place that reads an attestation's dates
// against each other. Its own callers screen some of those shapes first, so
// the relation is pinned here directly: a later caller that screens less must
// still meet the same refusals.

func TestEvidenceWithNoCheckTimeIsRefusedByTheRelationItself(t *testing.T) {
	err := checkObservesItsEvidence(BackupAttestation{
		LatestFullBackup: time.Now().UTC().Add(-time.Hour),
		RestoreDrillAt:   time.Now().UTC().Add(-2 * time.Hour),
	})
	if err == nil {
		t.Fatal("an attestation with no check time was accepted")
	}
	if !strings.Contains(err.Error(), "no check time") {
		t.Fatalf("the missing check time was not named: %v", err)
	}
}

// The monitor cannot have observed either fact after it looked, so evidence
// dated past the check time is incoherent rather than merely imprecise. Both
// subjects are named separately, which is what tells an operator which clock
// to go and look at.
func TestEvidenceObservedAfterTheCheckIsRefusedBySubject(t *testing.T) {
	checkedAt := time.Now().UTC()
	for subject, attestation := range map[string]BackupAttestation{
		"full backup": {
			CheckedAt:        checkedAt,
			LatestFullBackup: checkedAt.Add(backupClockSkew + time.Minute),
			RestoreDrillAt:   checkedAt.Add(-time.Hour),
		},
		"restore drill": {
			CheckedAt:        checkedAt,
			LatestFullBackup: checkedAt.Add(-time.Hour),
			RestoreDrillAt:   checkedAt.Add(backupClockSkew + time.Minute),
		},
	} {
		err := checkObservesItsEvidence(attestation)
		if err == nil {
			t.Fatalf("%s dated after the check was accepted", subject)
		}
		if !strings.Contains(err.Error(), subject) {
			t.Fatalf("%s was not named as the incoherent evidence: %v", subject, err)
		}
	}
}

// The skew allowance is what keeps two well-run machines from failing the
// gate over seconds, so it is pinned on both sides of the bound rather than
// left to drift with the constant.
func TestTheSkewAllowanceHoldsOnBothSides(t *testing.T) {
	checkedAt := time.Now().UTC()
	within := BackupAttestation{
		CheckedAt:        checkedAt,
		LatestFullBackup: checkedAt.Add(backupClockSkew),
		RestoreDrillAt:   checkedAt.Add(-time.Hour),
	}
	if err := checkObservesItsEvidence(within); err != nil {
		t.Fatalf("evidence exactly at the skew allowance was refused: %v", err)
	}
	beyond := within
	beyond.LatestFullBackup = checkedAt.Add(backupClockSkew + time.Second)
	if err := checkObservesItsEvidence(beyond); err == nil {
		t.Fatal("evidence one second past the skew allowance was accepted")
	}
}
