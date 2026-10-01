package board

import (
	"testing"
	"time"
)

// heldSnapshot is a board in a live phase with one holder, built so a test can
// move only the phase and the yield stamp.
func heldSnapshot(phase Phase, stamp time.Time) Snapshot {
	granted := time.Date(2026, 9, 26, 9, 0, 0, 0, time.UTC)
	lease := &Lease{
		ID:                "lease-1",
		WaiterID:          "waiter-1",
		Holder:            "ci-runner-3",
		Class:             ClassCI,
		Reason:            "bench debug",
		Generation:        4,
		GrantedAt:         granted,
		ExpiresAt:         granted.Add(30 * time.Minute),
		RequestedDuration: 30 * time.Minute,
		DeadlineVersion:   1,
		YieldRequestedAt:  stamp,
	}
	highWater := uint64(4)
	if phase == GrantPending {
		highWater = 3
	}
	return Snapshot{
		BoardID:        "board-a",
		Phase:          phase,
		Generation:     4,
		AgentHighWater: highWater,
		Version:        9,
		Lease:          lease,
	}
}

func TestAStampInAPhaseThatAskedForNoneIsRefused(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	for _, phase := range []Phase{GrantPending, Active} {
		err := Validate(heldSnapshot(phase, stamp))
		if err == nil {
			t.Fatalf("phase %s: retained yield stamp accepted", phase)
		}
		if !IsCode(err, Conflict) {
			t.Fatalf("phase %s: code = %v, want conflict", phase, err)
		}
	}
}

func TestTheRefusalNamesThePhaseThatCarriedTheStamp(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	err := Validate(heldSnapshot(Active, stamp))
	if err == nil {
		t.Fatal("retained yield stamp accepted")
	}
	// The phase is the whole finding: an operator reading the refusal needs
	// to know which of the two disagreeing facts to go and check.
	if got := err.Error(); got == "" || !contains(got, "active") {
		t.Fatalf("detail = %q, want it to name the phase", got)
	}
}

func TestAYieldPhaseKeepsItsStamp(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	for _, phase := range []Phase{YieldRequested, Draining} {
		if err := Validate(heldSnapshot(phase, stamp)); err != nil {
			t.Fatalf("phase %s: %v", phase, err)
		}
	}
}

func TestALiveNonYieldPhaseWithoutAStampIsOrdinary(t *testing.T) {
	for _, phase := range []Phase{GrantPending, Active} {
		if err := Validate(heldSnapshot(phase, time.Time{})); err != nil {
			t.Fatalf("phase %s: %v", phase, err)
		}
	}
}

// The recovery phases retain the lease as evidence and the outstanding request
// is part of that evidence: it is what lets YieldSampleFor record the censored
// sample naming recovery or quarantine as the reason the handoff never
// completed.
func TestARecoveryPhaseKeepsAnOutstandingRequestAsEvidence(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	for _, phase := range []Phase{RecoveryRequired, Recovering, Quarantined} {
		s := heldSnapshot(phase, stamp)
		if err := Validate(s); err != nil {
			t.Fatalf("phase %s: %v", phase, err)
		}
		// And the sample it exists for is still derivable.
		sample, ok, err := YieldSampleFor(s, []Event{{
			Kind: RecoveryNeeded, At: stamp.Add(time.Minute), BoardID: s.BoardID,
			Actor: "server", LeaseID: s.Lease.ID,
		}}, cohortFor(s.BoardID), 0)
		if err != nil || !ok {
			t.Fatalf("phase %s: sample not derived: ok=%v err=%v", phase, ok, err)
		}
		if sample.ExclusionReason == "" {
			t.Fatalf("phase %s: censored sample lost its reason", phase)
		}
	}
}

func TestTheCheckIsIndifferentToAFreeBoard(t *testing.T) {
	if err := checkYieldStampNeedsAYieldPhase(Ready, nil); err != nil {
		t.Fatalf("free board refused: %v", err)
	}
}

// The rule runs over every phase exactly once, so the accept and refuse sets
// are stated together rather than drifting apart as phases are added.
func TestEveryPhaseIsDecidedOnce(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	refused := map[Phase]bool{GrantPending: true, Active: true}
	all := []Phase{Ready, GrantPending, Active, YieldRequested, Draining, RecoveryRequired, Recovering, Quarantined}
	for _, phase := range all {
		lease := heldSnapshot(phase, stamp).Lease
		err := checkYieldStampNeedsAYieldPhase(phase, lease)
		if refused[phase] != (err != nil) {
			t.Fatalf("phase %s: err = %v, refused = %v", phase, err, refused[phase])
		}
	}
}

// The existing rule is the other direction of the same disagreement; neither
// stands in for the other.
func TestTheConverseRuleStillHolds(t *testing.T) {
	for _, phase := range []Phase{YieldRequested, Draining} {
		err := Validate(heldSnapshot(phase, time.Time{}))
		if err == nil {
			t.Fatalf("phase %s: yield phase without a stamp accepted", phase)
		}
	}
}

// What the stale stamp actually buys, caught at the two readers that never ask
// the phase. Both are shown against the snapshot Validate now refuses.
func TestTheReadersTreatTheStaleStampAsAHandoffInFlight(t *testing.T) {
	stamp := time.Date(2026, 9, 26, 9, 5, 0, 0, time.UTC)
	s := heldSnapshot(Active, stamp)
	s.Lease.HandoffTarget = 20 * time.Second
	s.Lease.HandoffCohort = cohortFor(s.BoardID)
	s.Queue = []Waiter{{
		ID: "waiter-2", LeaseID: "lease-2", Holder: "human-op", Class: ClassHuman,
		Reason: "bench", Duration: time.Hour, Sequence: 1,
		QueuedAt: s.Lease.GrantedAt.Add(time.Minute),
	}}
	s.NextSequence = 1

	// PlanYield would report a handoff outstanding, anchored to the stale
	// stamp rather than now, and already overdue against a promise nobody
	// was given.
	now := stamp.Add(10 * time.Minute)
	if _, err := PlanYield(s, "waiter-2", YieldOperator, cohortFor(s.BoardID),
		DeclaredHandoffBounds{SafeStepBound: 5 * time.Second, RestoreProbeBound: 5 * time.Second},
		nil, now); err == nil {
		t.Fatal("PlanYield accepted a board whose phase asked for no yield")
	}

	// YieldSampleFor would file a completed latency measured from it.
	if _, _, err := YieldSampleFor(s, []Event{{
		Kind: LeaseReleased, At: now, BoardID: s.BoardID, Actor: "ci-runner-3", LeaseID: s.Lease.ID,
	}}, cohortFor(s.BoardID), 0); err != nil {
		// The sample derivation does not run Validate itself; this call is
		// here to state that it is Validate, not the reader, that stops the
		// phantom handoff.
		t.Fatalf("unexpected sample error: %v", err)
	}
}

func cohortFor(boardID string) YieldCohort {
	return YieldCohort{
		BoardID:         boardID,
		BoardModel:      "ra8p1-evk",
		FixtureRevision: "fx-12",
		TaskName:        "flash-and-probe",
		CatalogDigest:   "sha256:catalog",
	}
}

func contains(haystack, needle string) bool {
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
