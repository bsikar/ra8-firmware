// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"errors"
	"fmt"
	"time"
)

// The attestation's three timestamps were each held against the verifier's
// own clock and never against each other, which left the one relation the
// envelope actually asserts unchecked.
//
// CheckedAt is not a third fact beside the other two. It is the moment the
// privileged monitor looked, and LatestFullBackup and RestoreDrillAt are what
// it saw when it looked: RefreshBackupAttestation runs pgBackRest, reads the
// restore drill receipt, and only then stamps CheckedAt with its own clock.
// So an envelope reporting a backup or a drill that finished after the check
// describes something the monitor could not have observed. Every field is
// signed, so this is not tampering; it is a receipt that disagrees with
// itself, and the likeliest cause is the externally authored restore drill
// receipt carrying a forward date, which nothing on the monitor's path
// refuses today.
//
// Why this is worth a refusal rather than a tidy-up: maxDrillAge runs to a
// year and maxBackupAge to a week, while maxCheckAge is capped at an hour. A
// drill dated forward buys the whole of maxDrillAge measured from a moment
// the drill had not happened yet, and the window that is supposed to be the
// strict one, CheckedAt, keeps passing because the monitor re-signs every few
// minutes. The gate would go on reserving runner capacity on restore evidence
// that had quietly expired.
//
// The skew allowance is the same one validAttestationTime already grants
// against the verifier's clock, and for the same reason: pgBackRest reports
// the database host's clock and the drill receipt is written by whatever ran
// the drill, so two well-run machines can still disagree by seconds. It is
// named here so both relations read it from one place.

// backupClockSkew is how far a timestamp may sit ahead of the clock judging
// it before the evidence is incoherent rather than merely imprecise.
const backupClockSkew = 2 * time.Minute

// checkObservesItsEvidence holds an attestation to the relation its own shape
// asserts: the monitor cannot have observed either fact after it looked.
// Absent timestamps are named here too, so a caller reaching this has one
// place to read for what is wrong with the envelope's dates.
func checkObservesItsEvidence(attestation BackupAttestation) error {
	if attestation.CheckedAt.IsZero() {
		return errors.New("attestation carries no check time to judge its evidence against")
	}
	observations := []struct {
		subject string
		at      time.Time
	}{
		{"full backup", attestation.LatestFullBackup},
		{"restore drill", attestation.RestoreDrillAt},
	}
	for _, observation := range observations {
		if observation.at.IsZero() {
			return fmt.Errorf("%s evidence is absent", observation.subject)
		}
		if observation.at.After(attestation.CheckedAt.Add(backupClockSkew)) {
			return fmt.Errorf("%s evidence is dated %s, after the check at %s that claims to have observed it",
				observation.subject, observation.at.UTC().Format(time.RFC3339),
				attestation.CheckedAt.UTC().Format(time.RFC3339))
		}
	}
	return nil
}
