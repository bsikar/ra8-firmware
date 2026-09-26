package board

import (
	"fmt"
	"testing"
	"time"
)

// A yield request is a promise as well as a transition: the requester is shown
// an ETA before the board is asked, and nothing recorded that number, so the
// handoff could only ever be judged against whatever the estimator said by the
// time it ended. These tests pin the recorded target, the invariants that keep
// a stale one from surviving, and the two readers that must agree with it.

func targetTestSnapshot(now time.Time) Snapshot {
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

func TestRequestYieldRecordsTheTargetTheRequesterWasShown(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := targetTestSnapshot(now)
	after, events, err := Apply(before, RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	if after.Phase != YieldRequested || after.Lease.YieldRequestedAt != now {
		t.Fatalf("phase=%s requested_at=%s", after.Phase, after.Lease.YieldRequestedAt)
	}
	if after.Lease.HandoffTarget != 45*time.Second {
		t.Fatalf("handoff target = %s, want the shown 45s", after.Lease.HandoffTarget)
	}
	if before.Lease.HandoffTarget != 0 {
		t.Fatalf("the caller's snapshot was mutated: %s", before.Lease.HandoffTarget)
	}
	if err := Validate(after); err != nil {
		t.Fatalf("recorded target left the board invalid: %v", err)
	}
	var asked bool
	for _, e := range events {
		if e.Kind == YieldAsked {
			asked = true
		}
	}
	if !asked {
		t.Fatal("no yield_asked event")
	}
}

func TestRepeatRequestCannotMoveAPromiseAlreadyMade(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("first request: %v", err)
	}
	later := now.Add(30 * time.Second)
	again, _, err := Apply(asked, RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 10 * time.Minute}, later)
	if err != nil {
		t.Fatalf("repeat request: %v", err)
	}
	if again.Lease.HandoffTarget != 45*time.Second {
		t.Fatalf("handoff target = %s, a repeat request overwrote the promise", again.Lease.HandoffTarget)
	}
	if again.Lease.YieldRequestedAt != now {
		t.Fatalf("requested_at = %s, want the first request at %s", again.Lease.YieldRequestedAt, now)
	}
}

func TestRequestYieldRefusesATargetOutOfRange(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	for _, target := range []time.Duration{-time.Second, MaxHandoffBound + time.Nanosecond} {
		if _, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: target}, now); !IsCode(err, InvalidArgument) {
			t.Fatalf("target %s: err = %v, want invalid_argument", target, err)
		}
	}
	// The bound itself is allowed: the estimator clamps to it, so a task
	// sitting at the ceiling must still be able to record what it was shown.
	after, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: MaxHandoffBound}, now)
	if err != nil || after.Lease.HandoffTarget != MaxHandoffBound {
		t.Fatalf("target at the ceiling refused: %v (%s)", err, after.Lease.HandoffTarget)
	}
}

func TestWithdrawnYieldClearsTheTargetWithTheRequest(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	cleared, _, err := Apply(asked, CancelWaiter{Actor: "brighton", WaiterID: "waiter-human"}, now.Add(time.Second))
	if err != nil {
		t.Fatalf("cancel waiter: %v", err)
	}
	if cleared.Phase != Active || !cleared.Lease.YieldRequestedAt.IsZero() {
		t.Fatalf("phase=%s requested_at=%s", cleared.Phase, cleared.Lease.YieldRequestedAt)
	}
	if cleared.Lease.HandoffTarget != 0 {
		t.Fatalf("handoff target = %s, a withdrawn yield left its promise behind", cleared.Lease.HandoffTarget)
	}
	if err := Validate(cleared); err != nil {
		t.Fatalf("cleared board invalid: %v", err)
	}
}

func TestValidateRefusesATargetNoRequestStandsBehind(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	s := targetTestSnapshot(now)
	s.Lease.HandoffTarget = 45 * time.Second
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("target without a request: err = %v, want conflict", err)
	}
	s.Lease.YieldRequestedAt = now
	s.Phase = YieldRequested
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("target with no cohort behind it: err = %v, want conflict", err)
	}
	s.Lease.HandoffCohort = cohortUnderTest()
	if err := Validate(s); err != nil {
		t.Fatalf("target with a request and cohort behind it refused: %v", err)
	}
	s.Lease.HandoffTarget = MaxHandoffBound + time.Nanosecond
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("out-of-range target: err = %v, want conflict", err)
	}
	s.Lease.HandoffTarget = -time.Second
	if err := Validate(s); !IsCode(err, Conflict) {
		t.Fatalf("negative target: err = %v, want conflict", err)
	}
}

func TestPlanYieldReportsThePromiseRatherThanARevisedEstimate(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	bounds := DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second}

	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	// History that would estimate high arrives mid-handoff. The estimate is
	// still reported honestly; the target the requester is held to is not.
	samples := make([]YieldSample, 0, 6)
	for i := 0; i < 6; i++ {
		samples = append(samples, YieldSample{
			Cohort:      cohort,
			LeaseID:     fmt.Sprintf("lease-old-%d", i),
			WaiterID:    "waiter-old",
			RequestedAt: now.Add(-time.Duration(i+1) * time.Hour),
			NeutralAt:   now.Add(-time.Duration(i+1)*time.Hour + 9*time.Minute),
		})
	}
	plan, err := PlanYield(asked, "waiter-human", YieldOperator, cohort, bounds, samples, now.Add(20*time.Second))
	if err != nil {
		t.Fatalf("plan yield: %v", err)
	}
	if !plan.Outstanding || plan.RequestedAt != now {
		t.Fatalf("outstanding=%v requested_at=%s", plan.Outstanding, plan.RequestedAt)
	}
	if plan.Estimate.Source != HandoffAsPromised || plan.ShownTarget() != 45*time.Second {
		t.Fatalf("source=%s target=%s, want the promised 45s", plan.Estimate.Source, plan.ShownTarget())
	}
	if want := now.Add(45 * time.Second); !plan.ExpectedNeutralAt.Equal(want) {
		t.Fatalf("expected neutral at %s, want %s", plan.ExpectedNeutralAt, want)
	}
	if plan.Estimate.Samples != 6 {
		t.Fatalf("samples = %d, the estimate behind the promise was discarded", plan.Estimate.Samples)
	}
	if plan.Overdue(now.Add(44 * time.Second)) {
		t.Fatal("overdue before the promised target")
	}
	if !plan.Overdue(now.Add(46 * time.Second)) {
		t.Fatal("not overdue past the promised target")
	}
}

func TestPlanYieldCarriesThePromiseEvenWithNoBoundsLeft(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohortUnderTest(), ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	// The task's declared bounds are gone by the time the requester polls, so
	// a fresh plan knows nothing. The promise already made still stands.
	plan, err := PlanYield(asked, "waiter-human", YieldOperator, cohort, DeclaredHandoffBounds{}, nil, now.Add(time.Second))
	if err != nil {
		t.Fatalf("plan yield: %v", err)
	}
	if !plan.Known() || plan.ShownTarget() != 45*time.Second || plan.Estimate.Source != HandoffAsPromised {
		t.Fatalf("known=%v target=%s source=%s", plan.Known(), plan.ShownTarget(), plan.Estimate.Source)
	}
}

func TestPlanYieldWithNoPromiseRecordedIsUnchanged(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	bounds := DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second}
	plan, err := PlanYield(targetTestSnapshot(now), "waiter-human", YieldAutomatic, cohort, bounds, nil, now)
	if err != nil {
		t.Fatalf("plan yield: %v", err)
	}
	if plan.Outstanding {
		t.Fatal("a board not yet asked reported an outstanding handoff")
	}
	if plan.Estimate.Source != HandoffFromDeclaredBounds || plan.ShownTarget() != bounds.SafetyBound() {
		t.Fatalf("source=%s target=%s, want the declared bound", plan.Estimate.Source, plan.ShownTarget())
	}
}

func TestEstimateHandoffNeverReportsThePromisedSource(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	bounds := DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second}
	estimate, err := EstimateHandoff(cohort, bounds, nil, now)
	if err != nil {
		t.Fatalf("estimate: %v", err)
	}
	if estimate.Source == HandoffAsPromised {
		t.Fatal("the estimator invented a promise nobody made")
	}
}

func TestYieldSampleIsMeasuredAgainstTheRecordedPromise(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohort, ShownTarget: 45 * time.Second}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	events := []Event{{Kind: LeaseReleased, LeaseID: asked.Lease.ID, At: now.Add(70 * time.Second), Actor: "ci"}}

	// A caller that knows nothing about the promise still measures against it.
	sample, ok, err := YieldSampleFor(asked, events, cohort, 0)
	if err != nil || !ok {
		t.Fatalf("sample: ok=%v err=%v", ok, err)
	}
	if !sample.SafetyOverrun || sample.Latency() != 70*time.Second {
		t.Fatalf("overrun=%v latency=%s, want an overrun of the recorded 45s", sample.SafetyOverrun, sample.Latency())
	}

	// So does a caller passing the same number back.
	if sample, ok, err = YieldSampleFor(asked, events, cohort, 45*time.Second); err != nil || !ok || !sample.SafetyOverrun {
		t.Fatalf("agreeing caller: ok=%v overrun=%v err=%v", ok, sample.SafetyOverrun, err)
	}

	// A caller passing a different one is refused, not quietly overruled.
	if _, _, err = YieldSampleFor(asked, events, cohort, 10*time.Minute); !IsCode(err, InvalidArgument) {
		t.Fatalf("contradicting caller: err = %v, want invalid_argument", err)
	}
}

func TestPlanAndSampleAgreeOnTheNumberThroughTheLease(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	cohort := YieldCohort{BoardID: "board-1", BoardModel: "ra8p1", FixtureRevision: "rev-c", TaskName: "hil-smoke", CatalogDigest: "digest-1"}
	bounds := DeclaredHandoffBounds{SafeStepBound: 20 * time.Second, RestoreProbeBound: 10 * time.Second}

	// The whole loop as the server drives it: plan, commit the plan's target
	// with the request, then record the sample from the committed lease.
	planned, err := PlanYield(targetTestSnapshot(now), "waiter-human", YieldOperator, cohort, bounds, nil, now)
	if err != nil {
		t.Fatalf("plan yield: %v", err)
	}
	asked, _, err := Apply(targetTestSnapshot(now), RequestYield{Actor: "brighton", WaiterID: "waiter-human", Cohort: cohort, ShownTarget: planned.ShownTarget()}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	if asked.Lease.HandoffTarget != planned.ShownTarget() {
		t.Fatalf("lease target %s != planned %s", asked.Lease.HandoffTarget, planned.ShownTarget())
	}
	events := []Event{{Kind: LeaseReleased, LeaseID: asked.Lease.ID, At: now.Add(planned.ShownTarget() - time.Second), Actor: "ci"}}
	sample, ok, err := YieldSampleFor(asked, events, cohort, 0)
	if err != nil || !ok {
		t.Fatalf("sample: ok=%v err=%v", ok, err)
	}
	if sample.SafetyOverrun || !sample.Completed() {
		t.Fatalf("a handoff inside the promise was recorded as overrun=%v completed=%v", sample.SafetyOverrun, sample.Completed())
	}
	if sample.RequestedAt != planned.RequestedAt {
		t.Fatalf("sample anchored at %s, plan at %s", sample.RequestedAt, planned.RequestedAt)
	}
}
