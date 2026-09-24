package board

import (
	"strings"
	"testing"
	"time"
)

// plannableBoard drives a CI lease to active with a human queued behind it,
// and returns the snapshot plus that human's waiter ID. Enqueueing the human
// also asks for the yield, so the caller gets an outstanding request; the
// active-only case is built separately where it matters.
func plannableBoard(t *testing.T) (Snapshot, string, time.Time) {
	t.Helper()
	s, asked := yieldedBoard(t)
	return s, "human-one", asked
}

func TestPlanYieldAnswersFromComparableHistory(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	now := asked.Add(2 * time.Second)
	cohort, bounds := testCohort(), testBounds()
	samples := completedSamples(cohort, now, 30, 34, 36, 40, 44, 90)

	plan, err := PlanYield(s, waiter, YieldAutomatic, cohort, bounds, samples, now)
	if err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	if !plan.Known() || plan.Estimate.Source != HandoffFromHistory {
		t.Fatalf("estimate did not come from history: %#v", plan.Estimate)
	}
	// p95 of six samples is the sixth by nearest rank, plus the margin.
	want := 90*time.Second + HandoffMargin
	if plan.Estimate.Target != want || plan.ShownTarget() != want {
		t.Fatalf("target = %s, want %s", plan.Estimate.Target, want)
	}
	if !plan.Outstanding {
		t.Fatal("a board already in yield_requested planned as a fresh request")
	}
	// THE ANCHOR: the ETA hangs off the moment the board was asked, not off
	// this call, so polling cannot push the deadline away from the requester.
	if !plan.RequestedAt.Equal(asked) {
		t.Fatalf("anchored at %s, want the request at %s", plan.RequestedAt, asked)
	}
	if !plan.ExpectedNeutralAt.Equal(asked.Add(want)) {
		t.Fatalf("expected neutral %s, want %s", plan.ExpectedNeutralAt, asked.Add(want))
	}
	later, err := PlanYield(s, waiter, YieldAutomatic, cohort, bounds, samples, now.Add(time.Minute))
	if err != nil || !later.ExpectedNeutralAt.Equal(plan.ExpectedNeutralAt) {
		t.Fatalf("deadline slid on a second poll: %s then %s (%v)", plan.ExpectedNeutralAt, later.ExpectedNeutralAt, err)
	}

	// Cohort, sample count and age reach the requester, per the architecture
	// note: an ETA with no provenance is not an answer.
	explained := plan.Explain(now)
	for _, want := range []string{"6 samples", "p95", cohort.BoardID, cohort.FixtureRevision, cohort.TaskName} {
		if !strings.Contains(explained, want) {
			t.Fatalf("provenance %q omits %q", explained, want)
		}
	}
}

func TestPlanYieldRejectsAutomaticDispatchWithoutDeclaredBounds(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	now := asked.Add(time.Second)
	cohort := testCohort()
	// History alone must not rescue an undeclared task: plenty of comparable
	// samples, still no bounds.
	samples := completedSamples(cohort, now, 3, 3, 3, 3, 3, 3, 3)

	for _, bounds := range []DeclaredHandoffBounds{
		{},
		{SafeStepBound: 5 * time.Second},
		{RestoreProbeBound: 5 * time.Second},
	} {
		if _, err := PlanYield(s, waiter, YieldAutomatic, cohort, bounds, samples, now); !IsCode(err, InvalidArgument) {
			t.Fatalf("automatic dispatch admitted with bounds %#v: %v", bounds, err)
		}

		// An operator asking for their own board back is told the ETA is
		// unknown rather than refused, and is never quietly handed the
		// thirty-second target as if it were this task's bound.
		plan, err := PlanYield(s, waiter, YieldOperator, cohort, bounds, samples, now)
		if err != nil {
			t.Fatalf("operator dispatch refused with bounds %#v: %v", bounds, err)
		}
		if plan.Known() || plan.Estimate.Source != HandoffUnknown {
			t.Fatalf("unknown ETA reported as known: %#v", plan.Estimate)
		}
		if plan.Estimate.Target != 0 || plan.ShownTarget() != 0 || !plan.ExpectedNeutralAt.IsZero() {
			t.Fatalf("unknown ETA carries a number: %#v", plan)
		}
		if plan.Overdue(now.Add(time.Hour)) {
			t.Fatal("a handoff with no shown target reported overdue")
		}
		if !strings.Contains(plan.Explain(now), "unknown") || !strings.Contains(plan.Explain(now), cohort.TaskName) {
			t.Fatalf("unknown ETA not explained: %q", plan.Explain(now))
		}
	}
}

func TestPlanYieldRefusesWhateverApplyWouldRefuse(t *testing.T) {
	cohort, bounds := testCohort(), testBounds()
	s, waiter, asked := plannableBoard(t)
	now := asked.Add(time.Second)

	// A waiter that is not queued, and one that does not outrank the holder,
	// are refused with the same codes Apply uses for RequestYield. The pairing
	// is what stops a plan quoting an ETA for a yield that cannot happen.
	ci := boardForTest(t)
	ci, _ = applyForTest(t, ci, request("ci-one", ClassCI), testEpoch)
	ci = ack(t, ci, testEpoch.Add(time.Second))
	ci, _ = applyForTest(t, ci, request("ci-two", ClassCI), testEpoch.Add(2*time.Second))

	cases := []struct {
		name   string
		state  Snapshot
		waiter string
		code   Code
	}{
		{"no waiter named", s, "", InvalidArgument},
		{"waiter not queued", s, "ghost", Denied},
		{"waiter does not outrank holder", ci, "ci-two", Denied},
		{"no holder to ask", boardForTest(t), "human-one", Conflict},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := PlanYield(tc.state, tc.waiter, YieldOperator, cohort, bounds, nil, now)
			if !IsCode(err, tc.code) {
				t.Fatalf("plan error = %v, want %s", err, tc.code)
			}
			// Apply agrees, on the same snapshot and waiter.
			_, _, applyErr := Apply(tc.state, RequestYield{Actor: "operator", WaiterID: tc.waiter}, now)
			if !IsCode(applyErr, tc.code) {
				t.Fatalf("Apply error = %v, want the same %s", applyErr, tc.code)
			}
		})
	}

	if _, err := PlanYield(s, waiter, "sideways", cohort, bounds, nil, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("unknown dispatch admitted: %v", err)
	}
	if _, err := PlanYield(s, waiter, YieldOperator, cohort, bounds, nil, time.Time{}); !IsCode(err, InvalidArgument) {
		t.Fatalf("plan built without a clock: %v", err)
	}
	if _, err := PlanYield(s, waiter, YieldOperator, YieldCohort{}, bounds, nil, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("plan built for an unidentifiable cohort: %v", err)
	}
}

func TestPlanYieldMeasuresTheOverrunAgainstWhatWasShown(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	now := asked.Add(time.Second)
	cohort, bounds := testCohort(), testBounds()
	// Below the sample floor, so the target is the declared safety bound: 20s.
	plan, err := PlanYield(s, waiter, YieldAutomatic, cohort, bounds, nil, now)
	if err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	if plan.Estimate.Source != HandoffFromDeclaredBounds || plan.ShownTarget() != bounds.SafetyBound() {
		t.Fatalf("declared bounds not used: %#v", plan.Estimate)
	}
	if plan.Overdue(asked.Add(bounds.SafetyBound())) {
		t.Fatal("exactly at the shown target reported overdue")
	}
	if !plan.Overdue(asked.Add(bounds.SafetyBound() + time.Nanosecond)) {
		t.Fatal("past the shown target not reported overdue")
	}

	// THE LOOP CLOSES HERE: the target the requester was shown is the target
	// the recorded sample is judged against, so an overrun in the history
	// means the promise was missed rather than some later estimate was.
	neutral := asked.Add(bounds.SafetyBound() + 5*time.Second)
	before := s
	_, events := applyForTest(t, before, Release{
		Actor: before.Lease.Holder, LeaseID: before.Lease.ID,
		Generation: before.Generation, NeutralReceipt: "neutral-proof",
	}, neutral)
	sample, ok, err := YieldSampleFor(before, events, cohort, plan.ShownTarget())
	if err != nil || !ok {
		t.Fatalf("no sample: %v %v", ok, err)
	}
	if !sample.SafetyOverrun || !sample.Completed() {
		t.Fatalf("overrun against the shown target not recorded: %#v", sample)
	}
	if sample.RequestedAt != plan.RequestedAt {
		t.Fatalf("sample anchored at %s, plan at %s", sample.RequestedAt, plan.RequestedAt)
	}
}

func TestPlanYieldForAFreshRequestAnchorsAtNow(t *testing.T) {
	// A human queued behind a human does not trigger the automatic yield, so
	// the board stays active with a same-class waiter; an operator may still
	// not ask, which is exactly the Denied case above. Use a CI holder with a
	// human waiter added by RequestYield-less enqueue instead: enqueue asks
	// automatically, so build the active case by hand.
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("ci-one", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	if s.Phase != Active {
		t.Fatalf("holder is not active: %s", s.Phase)
	}
	// Queue the human directly on the snapshot so no yield is asked for yet.
	s.Queue = append(s.Queue, Waiter{
		ID: "human-later", LeaseID: "lease-human-later", Holder: "person",
		Class: ClassHuman, Reason: "bench time", Duration: time.Hour,
		Sequence: s.NextSequence + 1, QueuedAt: testEpoch.Add(2 * time.Second),
	})
	s.NextSequence++

	now := testEpoch.Add(3 * time.Second)
	bounds := testBounds()
	plan, err := PlanYield(s, "human-later", YieldAutomatic, testCohort(), bounds, nil, now)
	if err != nil {
		t.Fatalf("plan refused: %v", err)
	}
	if plan.Outstanding {
		t.Fatal("a board that has not been asked reported as outstanding")
	}
	if !plan.RequestedAt.Equal(now) || !plan.ExpectedNeutralAt.Equal(now.Add(bounds.SafetyBound())) {
		t.Fatalf("fresh plan not anchored at now: %#v", plan)
	}
	// Not yet asked means not yet overdue, however long ago the ETA implies.
	if plan.Overdue(now.Add(time.Hour)) {
		t.Fatal("a yield that was never asked for reported overdue")
	}
}

func TestEstimateHandoffNeverReportsTheUnknownSource(t *testing.T) {
	now := time.Now()
	cohort, bounds := testCohort(), testBounds()
	for _, samples := range [][]YieldSample{
		nil,
		completedSamples(cohort, now, 4),
		completedSamples(cohort, now, 4, 5, 6, 7, 8, 9),
	} {
		estimate, err := EstimateHandoff(cohort, bounds, samples, now)
		if err != nil {
			t.Fatalf("estimate refused: %v", err)
		}
		if estimate.Source == HandoffUnknown {
			t.Fatal("the estimator reported an unknown source; only PlanYield may")
		}
	}
	// And it still refuses undeclared bounds outright rather than answering
	// with the unknown source.
	if _, err := EstimateHandoff(cohort, DeclaredHandoffBounds{}, nil, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("estimator accepted undeclared bounds: %v", err)
	}
}
