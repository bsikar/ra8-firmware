package board

import (
	"strings"
	"testing"
	"time"
)

// promisedBoard is an outstanding yield whose lease carries both halves of the
// promise, the shown target and the cohort it was estimated over, which is
// what RequestYield records and what PlanYield must be held to afterwards.
func promisedBoard(t *testing.T, target time.Duration, cohort YieldCohort) (Snapshot, string, time.Time) {
	t.Helper()
	s, waiter, asked := plannableBoard(t)
	if s.Lease == nil {
		t.Fatal("fixture has no lease")
	}
	lease := *s.Lease
	lease.HandoffTarget = target
	lease.HandoffCohort = cohort
	s.Lease = &lease
	if err := Validate(s); err != nil {
		t.Fatalf("fixture snapshot is invalid: %v", err)
	}
	return s, waiter, asked
}

// planCohort is the cohort the fixture board's lease actually names.
func planCohort(s Snapshot) YieldCohort {
	cohort := testCohort()
	cohort.BoardID = s.BoardID
	return cohort
}

func TestAPlanIsRefusedWhenItsCohortIsNotTheOneThePromiseWasMadeUnder(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)
	elsewhere := promised
	elsewhere.FixtureRevision = "fixture-d"

	s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
	now := asked.Add(2 * time.Second)

	if _, err := PlanYield(s, waiter, YieldAutomatic, elsewhere, testBounds(),
		completedSamples(elsewhere, now, 30, 34, 36, 40, 44, 90), now); !IsCode(err, InvalidArgument) {
		t.Fatalf("plan built on a cohort the promise was not made under: %v", err)
	}
}

// *** THE BOARD RULE DOES NOT COVER THIS ONE. A cohort naming the right board
// still walks past checkCohortNamesBoard, and the four fields it says nothing
// about are exactly the ones YieldCohort's doc comment says start a fresh
// history. Each is checked on its own so a later edit cannot narrow the rule
// to one field and keep this test passing.
func TestEveryCohortFieldThatStartsAFreshHistoryIsChecked(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)

	for _, differs := range []struct {
		field  string
		change func(*YieldCohort)
	}{
		{"board model", func(c *YieldCohort) { c.BoardModel = "ra8d1" }},
		{"fixture revision", func(c *YieldCohort) { c.FixtureRevision = "fixture-d" }},
		{"task name", func(c *YieldCohort) { c.TaskName = "hil-soak" }},
		{"catalog digest", func(c *YieldCohort) { c.CatalogDigest = strings.Repeat("b", 64) }},
		{"image digest", func(c *YieldCohort) { c.ImageSHA256 = "0e6f1d5c" }},
	} {
		derived := promised
		differs.change(&derived)
		if derived.BoardID != promised.BoardID {
			t.Fatalf("%s case changed the board, which another rule already refuses", differs.field)
		}
		if err := checkCohortNamesBoard(derived, promised.BoardID); err != nil {
			t.Fatalf("%s case is refused by the board rule, so it proves nothing here: %v", differs.field, err)
		}

		s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
		now := asked.Add(time.Second)
		if _, err := PlanYield(s, waiter, YieldAutomatic, derived, testBounds(), nil, now); !IsCode(err, InvalidArgument) {
			t.Fatalf("a cohort differing only in %s was admitted: %v", differs.field, err)
		}
	}
}

// The agreeing case is the ordinary one and must keep every property the plan
// promised before this rule existed: the pinned target, the request anchor,
// and the provenance the requester reads beside it.
func TestTheAgreeingCohortStillPlansAndStillPinsThePromise(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)
	s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
	now := asked.Add(3 * time.Second)

	plan, err := PlanYield(s, waiter, YieldAutomatic, promised, testBounds(),
		completedSamples(promised, now, 30, 34, 36, 40, 44, 90), now)
	if err != nil {
		t.Fatalf("the promised cohort was refused: %v", err)
	}
	if plan.Estimate.Source != HandoffAsPromised || plan.Estimate.Target != 45*time.Second {
		t.Fatalf("the recorded promise was not pinned: %#v", plan.Estimate)
	}
	if !plan.Outstanding || !plan.RequestedAt.Equal(asked) {
		t.Fatalf("plan lost its anchor: outstanding=%v at %s", plan.Outstanding, plan.RequestedAt)
	}
	if !plan.ExpectedNeutralAt.Equal(asked.Add(45 * time.Second)) {
		t.Fatalf("expected neutral %s, want %s", plan.ExpectedNeutralAt, asked.Add(45*time.Second))
	}
	if explained := plan.Explain(now); !strings.Contains(explained, promised.FixtureRevision) {
		t.Fatalf("provenance %q does not describe the promised cohort", explained)
	}
}

// *** A LEASE THAT RECORDED NO COHORT IS NOT A CONTRADICTION. Zero means none
// was recorded, per the Lease field's own doc comment, and a yield the state
// machine raised itself leaves exactly that. There is no promise to disagree
// with, so the plan proceeds on the caller's cohort as it always has.
func TestALeaseWithNoRecordedCohortStillPlans(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	if s.Lease.HandoffCohort != (YieldCohort{}) {
		t.Fatalf("fixture already records a cohort: %#v", s.Lease.HandoffCohort)
	}
	now := asked.Add(time.Second)
	cohort := planCohort(s)

	plan, err := PlanYield(s, waiter, YieldAutomatic, cohort, testBounds(),
		completedSamples(cohort, now, 30, 34, 36, 40, 44, 90), now)
	if err != nil {
		t.Fatalf("plan refused with no recorded cohort: %v", err)
	}
	if plan.Estimate.Source != HandoffFromHistory {
		t.Fatalf("estimate did not come from history: %#v", plan.Estimate)
	}
}

// The refusal is the one YieldSampleFor gives at the other end of the same
// handoff, deliberately in the same words: both are one caller's derived
// cohort disagreeing with the one on the lease.
func TestBothEndsOfTheHandoffRefuseTheDisagreementInTheSameWords(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)
	derived := promised
	derived.TaskName = "hil-soak"

	s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
	now := asked.Add(time.Second)
	_, planErr := PlanYield(s, waiter, YieldAutomatic, derived, testBounds(), nil, now)
	if planErr == nil {
		t.Fatal("the plan admitted the disagreement")
	}

	_, _, sampleErr := YieldSampleFor(s, []Event{{
		Kind: LeaseReleased, LeaseID: s.Lease.ID, At: now, Actor: "ci-one",
	}}, derived, 45*time.Second)
	if sampleErr == nil {
		t.Fatal("the sample admitted the disagreement")
	}
	if !strings.Contains(planErr.Error(), "contradicts the cohort recorded on the lease") ||
		!strings.Contains(sampleErr.Error(), "contradicts the cohort recorded on the lease") {
		t.Fatalf("the two ends word it differently: plan %q, sample %q", planErr, sampleErr)
	}
}

// The check is asked before the estimate is computed, so a disagreement is
// reported as itself rather than as whatever the wrong history happens to say.
func TestTheDisagreementIsReportedBeforeTheHistoryIsJudged(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)
	derived := promised
	derived.ImageSHA256 = "0e6f1d5c"

	s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
	now := asked.Add(time.Second)
	// History that EstimateHandoff would refuse outright: a row claiming a
	// completed handoff with no neutral time. The cohort refusal must win.
	unusable := []YieldSample{{Cohort: derived, RequestedAt: now.Add(-time.Hour)}}

	_, err := PlanYield(s, waiter, YieldAutomatic, derived, testBounds(), unusable, now)
	if err == nil || !strings.Contains(err.Error(), "contradicts the cohort recorded on the lease") {
		t.Fatalf("the history was judged before the promise: %v", err)
	}
}

// An operator asking with no declared bounds gets the unknown-ETA path, and
// that path must not be a way around the rule: it reads the lease too.
func TestTheUnknownEtaPathIsHeldToThePromisedCohortAsWell(t *testing.T) {
	base, _, _ := plannableBoard(t)
	promised := planCohort(base)
	derived := promised
	derived.FixtureRevision = "fixture-d"

	s, waiter, asked := promisedBoard(t, 45*time.Second, promised)
	now := asked.Add(time.Second)

	if _, err := PlanYield(s, waiter, YieldOperator, derived, DeclaredHandoffBounds{}, nil, now); !IsCode(err, InvalidArgument) {
		t.Fatalf("the unknown-ETA path admitted a foreign cohort: %v", err)
	}
}

func TestTheCheckItselfIsExactComparison(t *testing.T) {
	cohort := testCohort()
	if err := checkPlanCohortIsThePromisedOne(cohort, nil); err != nil {
		t.Fatalf("no lease is not a contradiction: %v", err)
	}
	if err := checkPlanCohortIsThePromisedOne(cohort, &Lease{}); err != nil {
		t.Fatalf("an unrecorded cohort is not a contradiction: %v", err)
	}
	if err := checkPlanCohortIsThePromisedOne(cohort, &Lease{HandoffCohort: cohort}); err != nil {
		t.Fatalf("the same cohort was refused: %v", err)
	}
	other := cohort
	other.BoardModel = "ra8d1"
	if err := checkPlanCohortIsThePromisedOne(cohort, &Lease{HandoffCohort: other}); !IsCode(err, InvalidArgument) {
		t.Fatalf("a differing cohort was admitted: %v", err)
	}
}
