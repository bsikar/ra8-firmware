package board

import (
	"testing"
	"time"
)

// selfRaisedYield returns a board whose yield was raised by the state machine
// itself: a higher-priority waiter arrived while the holder was active, so the
// lease carries the request stamp and no promise at all.
func selfRaisedYield(t *testing.T) (Snapshot, string) {
	t.Helper()
	start := time.Date(2026, 3, 2, 9, 0, 0, 0, time.UTC)
	s, err := New("board-a")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-ai", LeaseID: "l-ai", Holder: "ai", Class: ClassAI,
		Reason: "build", Duration: 30 * time.Minute,
	}}, start)
	if err != nil {
		t.Fatalf("enqueue holder: %v", err)
	}
	s, _, err = Apply(s, AcknowledgeGrant{
		Actor: "agent", LeaseID: s.Lease.ID, Generation: s.Generation, InstalledGeneration: s.Generation,
	}, start.Add(time.Second))
	if err != nil {
		t.Fatalf("acknowledge: %v", err)
	}
	s, _, err = Apply(s, Enqueue{Actor: "server", Waiter: Waiter{
		ID: "w-human", LeaseID: "l-human", Holder: "person", Class: ClassHuman,
		Reason: "debug", Duration: 20 * time.Minute,
	}}, start.Add(2*time.Second))
	if err != nil {
		t.Fatalf("enqueue human: %v", err)
	}
	if s.Phase != YieldRequested {
		t.Fatalf("phase = %q, want a yield raised by the enqueue", s.Phase)
	}
	if s.Lease.HandoffTarget != 0 || s.Lease.HandoffCohort != (YieldCohort{}) {
		t.Fatalf("a self-raised yield recorded a promise: %v %+v", s.Lease.HandoffTarget, s.Lease.HandoffCohort)
	}
	return s, "w-human"
}

func shownCohort(boardID string) YieldCohort {
	return YieldCohort{
		BoardID:         boardID,
		BoardModel:      "ek-ra8d2",
		FixtureRevision: "fx-7",
		TaskName:        "hil-smoke",
		CatalogDigest:   "cat-1",
		ImageSHA256:     "img-1",
	}
}

func TestOutstandingRequestAdoptsTheTargetItWasShown(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	cohort := shownCohort(s.BoardID)
	now := s.Lease.YieldRequestedAt.Add(time.Minute)

	next, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: cohort,
	}, now)
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}
	if next.Lease.HandoffTarget != 4*time.Minute {
		t.Fatalf("HandoffTarget = %v, want the target the requester was shown", next.Lease.HandoffTarget)
	}
	if next.Lease.HandoffCohort != cohort {
		t.Fatalf("HandoffCohort = %+v, want %+v", next.Lease.HandoffCohort, cohort)
	}
}

func TestAdoptingAPromiseDoesNotMoveTheRequestStamp(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	asked := s.Lease.YieldRequestedAt

	next, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: shownCohort(s.BoardID),
	}, asked.Add(5*time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}
	if !next.Lease.YieldRequestedAt.Equal(asked) {
		t.Fatalf("YieldRequestedAt = %v, want the moment the board was first asked %v", next.Lease.YieldRequestedAt, asked)
	}
	if next.Phase != YieldRequested {
		t.Fatalf("phase = %q, want it unchanged", next.Phase)
	}
}

func TestAdoptingAPromiseAsksNothingAgain(t *testing.T) {
	s, waiterID := selfRaisedYield(t)

	_, events, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: shownCohort(s.BoardID),
	}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}
	for _, e := range events {
		if e.Kind == YieldAsked {
			t.Fatalf("a second yield_requested event was emitted for one handoff: %+v", e)
		}
	}
}

func TestARecordedPromiseSurvivesALaterRequest(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	first := shownCohort(s.BoardID)

	s, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: first,
	}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("first RequestYield: %v", err)
	}

	second := first
	second.FixtureRevision = "fx-8"
	next, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 11 * time.Minute, Cohort: second,
	}, s.Lease.YieldRequestedAt.Add(2*time.Minute))
	if err != nil {
		t.Fatalf("second RequestYield: %v", err)
	}
	if next.Lease.HandoffTarget != 4*time.Minute {
		t.Fatalf("HandoffTarget = %v, want the first requester's promise kept", next.Lease.HandoffTarget)
	}
	if next.Lease.HandoffCohort != first {
		t.Fatalf("HandoffCohort = %+v, want the first requester's cohort kept", next.Lease.HandoffCohort)
	}
}

func TestAdoptedPromiseIsWhatTheNextPlanIsHeldTo(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	cohort := shownCohort(s.BoardID)
	shown := 4 * time.Minute

	s, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: shown, Cohort: cohort,
	}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}

	bounds := DeclaredHandoffBounds{SafeStepBound: 30 * time.Second, RestoreProbeBound: 30 * time.Second}
	plan, err := PlanYield(s, waiterID, YieldOperator, cohort, bounds, nil, s.Lease.YieldRequestedAt.Add(2*time.Minute))
	if err != nil {
		t.Fatalf("PlanYield: %v", err)
	}
	if plan.Estimate.Source != HandoffAsPromised {
		t.Fatalf("source = %q, want the recorded promise to win", plan.Estimate.Source)
	}
	if plan.Estimate.Target != shown {
		t.Fatalf("target = %v, want the number already shown %v", plan.Estimate.Target, shown)
	}
	if !plan.ExpectedNeutralAt.Equal(s.Lease.YieldRequestedAt.Add(shown)) {
		t.Fatalf("ExpectedNeutralAt = %v, want the promise measured from the request", plan.ExpectedNeutralAt)
	}
}

func TestAdoptedCohortIsWhereTheSampleIsFiled(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	cohort := shownCohort(s.BoardID)

	s, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: cohort,
	}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}

	before := s
	released := before.Lease.YieldRequestedAt.Add(2 * time.Minute)
	_, events, err := Apply(before, Release{
		Actor: "ai", LeaseID: before.Lease.ID, Generation: before.Generation, NeutralReceipt: "receipt-1",
	}, released)
	if err != nil {
		t.Fatalf("Release: %v", err)
	}

	elsewhere := cohort
	elsewhere.TaskName = "something-else"
	sample, ok, err := YieldSampleFor(before, events, elsewhere, 0)
	if err == nil {
		t.Fatalf("a cohort contradicting the adopted one was accepted: %+v %v", sample, ok)
	}

	sample, ok, err = YieldSampleFor(before, events, YieldCohort{}, 0)
	if err != nil || !ok {
		t.Fatalf("YieldSampleFor: %+v %v %v", sample, ok, err)
	}
	if sample.Cohort != cohort {
		t.Fatalf("sample cohort = %+v, want the adopted one %+v", sample.Cohort, cohort)
	}
}

func TestAdoptionLeavesALeaseWithNothingShownAlone(t *testing.T) {
	s, waiterID := selfRaisedYield(t)

	next, _, err := Apply(s, RequestYield{Actor: "operator", WaiterID: waiterID}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}
	if next.Lease.HandoffTarget != 0 || next.Lease.HandoffCohort != (YieldCohort{}) {
		t.Fatalf("a request that showed nothing recorded something: %v %+v", next.Lease.HandoffTarget, next.Lease.HandoffCohort)
	}
}

func TestAdoptedPromiseLeavesAValidSnapshot(t *testing.T) {
	s, waiterID := selfRaisedYield(t)

	next, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: shownCohort(s.BoardID),
	}, s.Lease.YieldRequestedAt.Add(time.Minute))
	if err != nil {
		t.Fatalf("RequestYield: %v", err)
	}
	if err := Validate(next); err != nil {
		t.Fatalf("Validate: %v", err)
	}
}

func TestAdoptionRefusesACohortForAnotherBoard(t *testing.T) {
	s, waiterID := selfRaisedYield(t)
	stranger := shownCohort("board-z")

	if _, _, err := Apply(s, RequestYield{
		Actor: "operator", WaiterID: waiterID, ShownTarget: 4 * time.Minute, Cohort: stranger,
	}, s.Lease.YieldRequestedAt.Add(time.Minute)); !IsCode(err, InvalidArgument) {
		t.Fatalf("err = %v, want the cohort held to the board even while adopting", err)
	}
}

func TestAdoptShownPromiseNeedsARequestOutstanding(t *testing.T) {
	lease := &Lease{ID: "l-1"}
	if adoptShownPromise(lease, time.Minute, shownCohort("board-a")) {
		t.Fatal("a lease with no yield request adopted a promise")
	}
	if adoptShownPromise(nil, time.Minute, shownCohort("board-a")) {
		t.Fatal("a nil lease adopted a promise")
	}
}

func TestAdoptShownPromiseKeepsACohortRecordedWithoutATarget(t *testing.T) {
	cohort := shownCohort("board-a")
	lease := &Lease{
		ID:               "l-1",
		YieldRequestedAt: time.Date(2026, 3, 2, 9, 0, 0, 0, time.UTC),
		HandoffCohort:    cohort,
	}
	other := cohort
	other.ImageSHA256 = "img-2"
	if adoptShownPromise(lease, time.Minute, other) {
		t.Fatal("a lease already carrying a cohort adopted another")
	}
	if lease.HandoffCohort != cohort {
		t.Fatalf("HandoffCohort = %+v, want the recorded one kept", lease.HandoffCohort)
	}
}
