package board

import (
	"strings"
	"testing"
	"time"
)

// A yield cohort must name the board it is about, on every path that records
// one and on the snapshot that carries it afterwards. PlanYield has always
// refused a foreign cohort; these hold Apply and Validate to the same rule,
// since a plan is advisory and RequestYield can be issued without one.

func foreignCohort() YieldCohort {
	cohort := cohortUnderTest()
	cohort.BoardID = "board-elsewhere"
	return cohort
}

func TestRequestYieldRefusesACohortForAnotherBoard(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)
	elsewhere := foreignCohort()
	if elsewhere.BoardID == before.BoardID {
		t.Fatalf("fixture cohort does not name another board")
	}

	_, _, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: elsewhere}, now)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("request yield accepted another board's cohort: %v", err)
	}
	if !strings.Contains(err.Error(), elsewhere.BoardID) {
		t.Fatalf("refusal does not name the cohort's board: %v", err)
	}
}

// The damaging half is the quiet one: a foreign cohort with no target still
// travels onto the lease and still decides where the completed measurement is
// filed, so it is refused with or without a promise attached.
func TestRequestYieldRefusesAForeignCohortWithNoShownTarget(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)

	if _, _, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		Cohort: foreignCohort()}, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("request yield accepted an unpromised foreign cohort: %v", err)
	}
}

func TestRequestYieldLeavesTheLeaseUntouchedWhenTheCohortIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)

	after, events, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: foreignCohort()}, now)
	if err == nil {
		t.Fatal("refused command still applied")
	}
	// A refusal is audited, as every denied action is, and nothing else.
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("refused command produced more than its denial audit: %+v", events)
	}
	if after.Phase != before.Phase || !after.Lease.YieldRequestedAt.IsZero() {
		t.Fatalf("refused command moved the board: phase=%v requested=%v", after.Phase, after.Lease.YieldRequestedAt)
	}
	if after.Lease.HandoffCohort != (YieldCohort{}) || after.Lease.HandoffTarget != 0 {
		t.Fatalf("refused cohort reached the lease: %+v", after.Lease)
	}
}

// The board's own cohort still records, which is what keeps the refusal from
// being a door nobody can walk through.
func TestRequestYieldStillAcceptsTheBoardsOwnCohort(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)
	cohort := cohortUnderTest()
	if cohort.BoardID != before.BoardID {
		t.Fatalf("fixture cohort %q is not the fixture board %q", cohort.BoardID, before.BoardID)
	}

	after, _, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: cohort}, now)
	if err != nil {
		t.Fatalf("request yield refused the board's own cohort: %v", err)
	}
	if after.Lease.HandoffCohort != cohort {
		t.Fatalf("recorded cohort = %+v, want %+v", after.Lease.HandoffCohort, cohort)
	}
}

func TestValidateRefusesARetainedCohortForAnotherBoard(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	s := cohortTestSnapshot(now)
	s.Phase = YieldRequested
	s.Lease.YieldRequestedAt = now
	s.Lease.HandoffTarget = 45 * time.Second
	s.Lease.HandoffCohort = cohortUnderTest()
	if err := Validate(s); err != nil {
		t.Fatalf("own-board fixture already invalid: %v", err)
	}

	s.Lease.HandoffCohort = foreignCohort()
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("snapshot kept a cohort for another board: %v", err)
	}
}

// The recovery phases keep the old lease as evidence, which is exactly where a
// bad row sits longest before something measures it.
func TestValidateRefusesARetainedForeignCohortInRecovery(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	for _, phase := range []Phase{RecoveryRequired, Quarantined} {
		s := cohortTestSnapshot(now)
		s.Phase = phase
		s.Lease.YieldRequestedAt = now
		s.Lease.HandoffTarget = 45 * time.Second
		s.Lease.HandoffCohort = foreignCohort()
		if err := Validate(s); !IsCode(err, Conflict) {
			t.Fatalf("%s kept a cohort for another board: %v", phase, err)
		}
	}
}

// One rule, one wording: the plan and the command refuse a foreign cohort in
// the same words, so an operator who saw the plan's refusal recognises the
// command's.
func TestThePlanAndTheCommandRefuseAForeignCohortIdentically(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)
	elsewhere := foreignCohort()

	_, applyErr := func() (Snapshot, error) {
		s, _, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
			ShownTarget: 45 * time.Second, Cohort: elsewhere}, now)
		return s, err
	}()
	if applyErr == nil {
		t.Fatal("command accepted a foreign cohort")
	}
	planErr := checkCohortNamesBoard(elsewhere, before.BoardID)
	if planErr == nil || planErr.Error() != applyErr.Error() {
		t.Fatalf("refusals differ: plan %v, command %v", planErr, applyErr)
	}
}
