package board

import (
	"strings"
	"testing"
	"time"
)

// A yield plan is estimated over one cohort and that cohort is recorded on the
// lease, so a cohort naming another board would both quote another board's
// history and file this handoff's measurement into it. These tests hold the
// plan to the board in the snapshot.

func TestPlanYieldRefusesACohortForAnotherBoard(t *testing.T) {
	s, waiter, _ := plannableBoard(t)
	bounds := testBounds()
	now := testEpoch.Add(3 * time.Second)

	elsewhere := testCohort()
	elsewhere.BoardID = "ra8p1-bench-7"
	if elsewhere.BoardID == s.BoardID {
		t.Fatalf("test cohort does not name another board")
	}

	for _, dispatch := range []YieldDispatch{YieldAutomatic, YieldOperator} {
		_, err := PlanYield(s, waiter, dispatch, elsewhere, bounds, nil, now)
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%s plan built over another board's cohort: %v", dispatch, err)
		}
		if !strings.Contains(err.Error(), elsewhere.BoardID) {
			t.Fatalf("refusal does not name the cohort's board: %v", err)
		}
	}
}

// Undeclared bounds are the path that returns a plan early, with the ETA
// reported unknown. The board check must still have happened by then: an
// unknown ETA is still recorded against a cohort.
func TestPlanYieldRefusesAForeignCohortEvenWithNoDeclaredBounds(t *testing.T) {
	s, waiter, _ := plannableBoard(t)
	elsewhere := testCohort()
	elsewhere.BoardID = "ra8p1-bench-7"

	if _, err := PlanYield(s, waiter, YieldOperator, elsewhere, DeclaredHandoffBounds{}, nil,
		testEpoch.Add(3*time.Second)); !IsCode(err, InvalidArgument) {
		t.Fatalf("plan with an unknown ETA accepted another board's cohort: %v", err)
	}
}

// The board's own cohort still plans normally, which is what keeps the new
// refusal from being a door nobody can walk through.
func TestPlanYieldAcceptsTheBoardsOwnCohort(t *testing.T) {
	s, waiter, asked := plannableBoard(t)
	cohort := testCohort()
	if cohort.BoardID != s.BoardID {
		t.Fatalf("fixture cohort %q is not the fixture board %q", cohort.BoardID, s.BoardID)
	}

	plan, err := PlanYield(s, waiter, YieldAutomatic, cohort, testBounds(), nil, asked.Add(time.Second))
	if err != nil {
		t.Fatalf("plan refused for the board's own cohort: %v", err)
	}
	if plan.Estimate.Cohort != cohort || !plan.Known() {
		t.Fatalf("plan lost its cohort or its ETA: %#v", plan.Estimate)
	}
}
