package board

import (
	"testing"
	"time"
)

// contendedLease is an honest extended lease: the deadline moved six minutes
// past the grant, all of it charged to the contended budget, which is inside
// both the class ceiling and the ten-minute contended cap Validate holds.
func contendedLease(now time.Time) *Lease {
	lease := deadlineLease(now)
	lease.DeadlineVersion = 2
	lease.ExpiresAt = lease.ExpiresAt.Add(6 * time.Minute)
	lease.ContendedExtensionUsed = 6 * time.Minute
	return lease
}

func TestALeaseThatSpentNoContendedBudgetIsAccepted(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	if err := checkContendedBudgetIsAccountedFor(deadlineLease(now)); err != nil {
		t.Fatalf("unextended lease refused: %v", err)
	}
	if err := checkContendedBudgetIsAccountedFor(nil); err != nil {
		t.Fatalf("nil lease refused: %v", err)
	}
}

func TestAChargeCoveredByTheExtensionsIsAccepted(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, spent := range []time.Duration{time.Nanosecond, time.Minute, 6 * time.Minute} {
		lease := contendedLease(now)
		lease.ContendedExtensionUsed = spent
		if err := checkContendedBudgetIsAccountedFor(lease); err != nil {
			t.Fatalf("charge of %s inside a 6m extension refused: %v", spent, err)
		}
	}
}

func TestAnUncontendedExtensionChargesNothingAndIsAccepted(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	lease := contendedLease(now)
	lease.ContendedExtensionUsed = 0
	if err := checkContendedBudgetIsAccountedFor(lease); err != nil {
		t.Fatalf("extended lease with no contended charge refused: %v", err)
	}
}

func TestBudgetSpentWithNoExtensionOnRecordIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, spent := range []time.Duration{time.Nanosecond, time.Second, 10 * time.Minute} {
		lease := deadlineLease(now)
		lease.ContendedExtensionUsed = spent
		err := checkContendedBudgetIsAccountedFor(lease)
		if !IsCode(err, Conflict) {
			t.Fatalf("charge of %s at version 1 accepted: %v", spent, err)
		}
		if got := err.Error(); got != "conflict: retained lease spent contended budget with no extension on record" {
			t.Fatalf("charge of %s: wrong detail %q", spent, got)
		}
	}
}

func TestAChargeBeyondEverySecondTheDeadlineMovedIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, version := range []uint64{2, 3, 97} {
		lease := contendedLease(now)
		lease.DeadlineVersion = version
		lease.ContendedExtensionUsed = 6*time.Minute + time.Nanosecond
		err := checkContendedBudgetIsAccountedFor(lease)
		if !IsCode(err, Conflict) {
			t.Fatalf("version %d: charge past the extended distance accepted: %v", version, err)
		}
		if got := err.Error(); got != "conflict: retained lease spent more contended budget than its deadline ever moved" {
			t.Fatalf("version %d: wrong detail %q", version, got)
		}
	}
}

func TestTheBoundIsTheDistanceFromTheGrantNotFromNow(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	lease := contendedLease(now)
	// The lease was granted well in the past and has run most of its
	// original duration; none of that is contended budget, and the rule
	// must not read the elapsed time as headroom.
	lease.ContendedExtensionUsed = extendedDistance(*lease) + time.Second
	if err := checkContendedBudgetIsAccountedFor(lease); !IsCode(err, Conflict) {
		t.Fatalf("charge measured against the wrong span: %v", err)
	}
	lease.ContendedExtensionUsed = extendedDistance(*lease)
	if err := checkContendedBudgetIsAccountedFor(lease); err != nil {
		t.Fatalf("charge exactly at the extended distance refused: %v", err)
	}
}

func TestANegativeChargeIsLeftToValidate(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	lease := deadlineLease(now)
	lease.ContendedExtensionUsed = -time.Minute
	if err := checkContendedBudgetIsAccountedFor(lease); err != nil {
		t.Fatalf("this rule judged a negative charge: %v", err)
	}
	snapshot := deadlineSnapshot(now)
	snapshot.Lease = lease
	if err := Validate(snapshot); !IsCode(err, Conflict) {
		t.Fatalf("Validate accepted a negative contended charge: %v", err)
	}
}

func TestAnUnaccountedChargeIsRefusedBySnapshotValidation(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	snapshot := deadlineSnapshot(now)
	snapshot.Lease.ContendedExtensionUsed = 5 * time.Minute
	err := Validate(snapshot)
	if !IsCode(err, Conflict) {
		t.Fatalf("Validate accepted a charge with no extension on record: %v", err)
	}
	if got := err.Error(); got != "conflict: retained lease spent contended budget with no extension on record" {
		t.Fatalf("wrong detail %q", got)
	}
}

func TestAnHonestlyExtendedSnapshotStillValidates(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	snapshot := deadlineSnapshot(now)
	snapshot.Lease = contendedLease(now)
	if err := Validate(snapshot); err != nil {
		t.Fatalf("honest extended lease refused: %v", err)
	}
}

func TestTheRuleIsHeldInEveryPhaseThatRetainsALease(t *testing.T) {
	now := time.Date(2026, 9, 26, 18, 0, 0, 0, time.UTC)
	for _, phase := range []Phase{Active, RecoveryRequired, Recovering, Quarantined} {
		snapshot := deadlineSnapshot(now)
		snapshot.Phase = phase
		snapshot.Lease.ContendedExtensionUsed = time.Minute
		if err := Validate(snapshot); !IsCode(err, Conflict) {
			t.Fatalf("phase %v accepted an unaccounted charge: %v", phase, err)
		}
	}
}
