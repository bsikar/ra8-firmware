package board

import (
	"testing"
	"time"
)

// A shown target names a number; the cohort names the work that number
// described. #1530 recorded the target on the lease and left the cohort to be
// re-derived whenever the handoff ended, so a board that had since changed
// task, image, or fixture would have absorbed the measurement of work it never
// did. These tests pin the cohort travelling with the promise, the invariants
// that keep a half-recorded promise off a lease, and the recorder binding to
// what was recorded rather than to what the caller believes now.

func cohortTestSnapshot(now time.Time) Snapshot {
	return Snapshot{
		BoardID:        "board-1",
		Phase:          Active,
		Generation:     3,
		AgentHighWater: 3,
		Version:        9,
		NextSequence:   2,
		Lease: &Lease{
			ID: "lease-held", WaiterID: "waiter-held", Holder: "ci", Class: ClassCI,
			Reason: "integration run", Generation: 3, GrantedAt: now.Add(-20 * time.Minute),
			ExpiresAt: now.Add(10 * time.Minute), RequestedDuration: 30 * time.Minute,
			DeadlineVersion: 1,
		},
		Queue: []Waiter{{
			ID: "waiter-human", LeaseID: "lease-human", Holder: "brighton", Class: ClassHuman,
			Reason: "bench debug", Duration: 30 * time.Minute, QueuedAt: now.Add(-time.Minute), Sequence: 1,
		}},
	}
}

func cohortUnderTest() YieldCohort {
	return YieldCohort{
		BoardID:         "board-1",
		BoardModel:      "ra8p1-ek",
		FixtureRevision: "fixture-c",
		TaskName:        "hil-smoke",
		CatalogDigest:   "2411656a6225954d8f8b6b4a593a79b0b95ddd080c5040515cb0a227f65216e5",
		ImageSHA256:     "ab" + "cd",
	}
}

func TestRequestYieldRecordsTheCohortItsTargetWasEstimatedOver(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := cohortTestSnapshot(now)
	cohort := cohortUnderTest()
	after, _, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: cohort}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	if after.Lease.HandoffCohort != cohort {
		t.Fatalf("recorded cohort = %+v, want %+v", after.Lease.HandoffCohort, cohort)
	}
	if after.Lease.HandoffTarget != 45*time.Second {
		t.Fatalf("handoff target = %s", after.Lease.HandoffTarget)
	}
	if before.Lease.HandoffCohort != (YieldCohort{}) {
		t.Fatalf("the caller's snapshot was mutated: %+v", before.Lease.HandoffCohort)
	}
	if err := Validate(after); err != nil {
		t.Fatalf("recorded cohort left the board invalid: %v", err)
	}
}

// A target with no cohort behind it cannot be measured against anything: no
// later reader can say which history it described, so no later reader can say
// it was missed. The pair is refused at the door rather than recorded half
// made.
func TestRequestYieldRefusesAPromiseWithNoCohortBehindIt(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	_, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton",
		WaiterID: "waiter-human", ShownTarget: 45 * time.Second}, now)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("err = %v, want invalid_argument", err)
	}
}

// The state machine raises a yield itself on enqueue and acknowledge, with no
// requester and no plan. That request shows no ETA and names no cohort, and
// both absences are the honest answer rather than a gap to fill.
func TestAYieldWithNoETAShownNeedsNoCohort(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	after, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human"}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	if after.Lease.HandoffCohort != (YieldCohort{}) || after.Lease.HandoffTarget != 0 {
		t.Fatalf("cohort=%+v target=%s, want both empty", after.Lease.HandoffCohort, after.Lease.HandoffTarget)
	}
	if err := Validate(after); err != nil {
		t.Fatalf("validate: %v", err)
	}
}

func TestRequestYieldRefusesACohortThatIdentifiesNothing(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := cohortUnderTest()
	cohort.FixtureRevision = ""
	_, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: cohort}, now)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("err = %v, want invalid_argument", err)
	}
}

func TestWithdrawnYieldClearsTheCohortWithTheRequest(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	asked, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: cohortUnderTest()}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	cleared, _, err := Apply(asked, CancelWaiter{Actor: "brighton", WaiterID: "waiter-human"}, now.Add(time.Second))
	if err != nil {
		t.Fatalf("cancel waiter: %v", err)
	}
	if cleared.Phase != Active {
		t.Fatalf("phase = %s, want active", cleared.Phase)
	}
	if cleared.Lease.HandoffCohort != (YieldCohort{}) || cleared.Lease.HandoffTarget != 0 {
		t.Fatalf("cohort=%+v target=%s survived the withdrawal", cleared.Lease.HandoffCohort, cleared.Lease.HandoffTarget)
	}
	if err := Validate(cleared); err != nil {
		t.Fatalf("validate: %v", err)
	}
}

// Validate is the last reader of a lease that came back from storage, where
// nothing stopped a hand-edited or half-migrated row carrying one half of the
// promise.
func TestValidateRefusesAHalfRecordedPromise(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	for _, tc := range []struct {
		name   string
		mutate func(*Lease)
	}{
		{"cohort with no request", func(l *Lease) { l.HandoffCohort = cohortUnderTest() }},
		{"target with no cohort", func(l *Lease) {
			l.YieldRequestedAt = now
			l.HandoffTarget = 45 * time.Second
		}},
		{"cohort that identifies nothing", func(l *Lease) {
			l.YieldRequestedAt = now
			l.HandoffTarget = 45 * time.Second
			c := cohortUnderTest()
			c.CatalogDigest = ""
			l.HandoffCohort = c
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			s := cohortTestSnapshot(now)
			tc.mutate(s.Lease)
			if err := Validate(s); !IsCode(err, Conflict) {
				t.Fatalf("err = %v, want conflict", err)
			}
		})
	}
}

// The recorder binds to what the request recorded, not to what its caller
// believes by the time the handoff ends.
func TestYieldSampleTakesTheCohortOffTheLease(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := cohortUnderTest()
	asked, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: 45 * time.Second, Cohort: cohort}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	released := []Event{{Kind: LeaseReleased, At: now.Add(30 * time.Second),
		BoardID: "board-1", Actor: "ci", LeaseID: "lease-held", Generation: 3}}

	sample, ok, err := YieldSampleFor(asked, released, YieldCohort{}, 0)
	if err != nil || !ok {
		t.Fatalf("sample: ok=%v err=%v", ok, err)
	}
	if sample.Cohort != cohort {
		t.Fatalf("sample cohort = %+v, want the recorded %+v", sample.Cohort, cohort)
	}
	if sample.Latency() != 30*time.Second || sample.SafetyOverrun {
		t.Fatalf("latency=%s overrun=%v", sample.Latency(), sample.SafetyOverrun)
	}

	// A caller naming the same cohort agrees and is accepted.
	if _, _, err := YieldSampleFor(asked, released, cohort, 0); err != nil {
		t.Fatalf("agreeing caller refused: %v", err)
	}

	// A caller naming a different one is refused rather than overruled: one
	// of the two is wrong about which work was measured, and which one it is
	// cannot be decided here.
	drifted := cohort
	drifted.ImageSHA256 = "ffff"
	if _, _, err := YieldSampleFor(asked, released, drifted, 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("err = %v, want invalid_argument", err)
	}
}

// A lease that recorded no cohort still takes one from its caller, which is
// every yield the state machine raised itself.
func TestYieldSampleStillAcceptsACallerCohortWhenTheLeaseRecordedNone(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	asked, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human"}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	released := []Event{{Kind: LeaseReleased, At: now.Add(12 * time.Second),
		BoardID: "board-1", Actor: "ci", LeaseID: "lease-held", Generation: 3}}
	sample, ok, err := YieldSampleFor(asked, released, cohortUnderTest(), 0)
	if err != nil || !ok {
		t.Fatalf("sample: ok=%v err=%v", ok, err)
	}
	if sample.Cohort != cohortUnderTest() {
		t.Fatalf("sample cohort = %+v", sample.Cohort)
	}
	if _, _, err := YieldSampleFor(asked, released, YieldCohort{}, 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("a sample with no cohort at all: err = %v, want invalid_argument", err)
	}
}

// The whole loop: plan an ETA over one cohort, commit both with the request,
// let the handoff end, and assert the sample the estimator will read back
// carries the same cohort AND the same number the requester was shown.
func TestPlanCommitAndSampleAgreeOnTheCohort(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := cohortUnderTest()
	bounds := DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second}

	plan, err := PlanYield(cohortTestSnapshot(now), "waiter-human", YieldOperator, cohort, bounds, nil, now)
	if err != nil {
		t.Fatalf("plan yield: %v", err)
	}
	asked, _, err := Apply(cohortTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human",
		ShownTarget: plan.ShownTarget(), Cohort: plan.Estimate.Cohort}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}

	overrun := plan.ShownTarget() + 5*time.Second
	released := []Event{{Kind: LeaseReleased, At: now.Add(overrun),
		BoardID: "board-1", Actor: "ci", LeaseID: "lease-held", Generation: 3}}
	sample, ok, err := YieldSampleFor(asked, released, YieldCohort{}, 0)
	if err != nil || !ok {
		t.Fatalf("sample: ok=%v err=%v", ok, err)
	}
	if sample.Cohort != plan.Estimate.Cohort {
		t.Fatalf("sample cohort = %+v, want the planned %+v", sample.Cohort, plan.Estimate.Cohort)
	}
	if !sample.SafetyOverrun {
		t.Fatalf("a handoff %s past the shown %s was not flagged", overrun, plan.ShownTarget())
	}
	if sample.RequestedAt != plan.RequestedAt {
		t.Fatalf("sample anchored at %s, plan at %s", sample.RequestedAt, plan.RequestedAt)
	}

	// And the sample is comparable to a fresh estimate over the same cohort:
	// the bucket it joins is the bucket whose number was quoted.
	estimate, err := EstimateHandoff(cohort, bounds, []YieldSample{sample}, now.Add(time.Minute))
	if err != nil {
		t.Fatalf("estimate over the recorded sample: %v", err)
	}
	if estimate.Cohort != sample.Cohort {
		t.Fatalf("estimate cohort = %+v, sample cohort = %+v", estimate.Cohort, sample.Cohort)
	}
}
