package server

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type recordingBudgetStore struct {
	cohort     board.YieldCohort
	bounds     board.DeclaredHandoffBounds
	samples    []board.YieldSample
	workErr    error
	historyErr error

	askedFor  board.YieldCohort
	askedAt   time.Time
	workCalls int
	readCalls int
}

func (s *recordingBudgetStore) HeldYieldWork(_ context.Context, _ board.Snapshot) (board.YieldCohort, board.DeclaredHandoffBounds, error) {
	s.workCalls++
	if s.workErr != nil {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, s.workErr
	}
	return s.cohort, s.bounds, nil
}

func (s *recordingBudgetStore) YieldHistory(_ context.Context, cohort board.YieldCohort, now time.Time) ([]board.YieldSample, error) {
	s.readCalls++
	s.askedFor, s.askedAt = cohort, now
	if s.historyErr != nil {
		return nil, s.historyErr
	}
	return s.samples, nil
}

func budgetCohort() board.YieldCohort {
	return board.YieldCohort{BoardID: "ek-ra8d2", BoardModel: "EK-RA8D2",
		FixtureRevision: "fixture-9", TaskName: "hil-uart", CatalogDigest: "digest-1"}
}

// The history read must ask for the cohort the held work derived, not for
// some bucket chosen before anyone looked at what the board is running.
func TestStoreYieldBudgetReadsHistoryForTheCohortTheWorkDerived(t *testing.T) {
	st := &recordingBudgetStore{
		cohort:  budgetCohort(),
		bounds:  board.DeclaredHandoffBounds{SafeStepBound: 18 * time.Second, RestoreProbeBound: 12 * time.Second},
		samples: []board.YieldSample{{Cohort: budgetCohort(), LeaseID: "lease-1"}},
	}
	budget, err := NewStoreYieldBudget(st).HandoffBudget(context.Background(), board.Snapshot{BoardID: "ek-ra8d2"})
	if err != nil {
		t.Fatalf("HandoffBudget: %v", err)
	}
	if st.askedFor != budgetCohort() {
		t.Fatalf("history read for %+v, want %+v", st.askedFor, budgetCohort())
	}
	if budget.Cohort != budgetCohort() || budget.Bounds.SafetyBound() != 30*time.Second || len(budget.Samples) != 1 {
		t.Fatalf("budget = %+v", budget)
	}
	if st.askedAt.IsZero() {
		t.Fatal("history read with a zero clock")
	}
}

// A board whose held work cannot be derived is not quoted an ETA over some
// other cohort's history: the read never happens at all.
func TestStoreYieldBudgetDoesNotReadHistoryWhenTheWorkIsUnknown(t *testing.T) {
	st := &recordingBudgetStore{workErr: store.ErrConflict}
	_, err := NewStoreYieldBudget(st).HandoffBudget(context.Background(), board.Snapshot{BoardID: "ek-ra8d2"})
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("err = %v, want ErrConflict", err)
	}
	if st.readCalls != 0 {
		t.Fatalf("history read %d times after the work was refused", st.readCalls)
	}
}

// A failed history read is a refusal, never an empty history. Silently
// estimating from declared bounds here would quote a person a number that
// says "no comparable history" when the truth is "we could not look".
func TestStoreYieldBudgetRefusesRatherThanEstimatingOverAFailedRead(t *testing.T) {
	st := &recordingBudgetStore{cohort: budgetCohort(), historyErr: store.ErrUnavailable}
	_, err := NewStoreYieldBudget(st).HandoffBudget(context.Background(), board.Snapshot{BoardID: "ek-ra8d2"})
	if !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("err = %v, want ErrUnavailable", err)
	}
}

func TestNewStoreYieldBudgetLeavesTheDoorClosedWithoutAStore(t *testing.T) {
	if b := NewStoreYieldBudget(nil); b != nil {
		t.Fatalf("provider = %v, want nil so the endpoint answers 503", b)
	}
	var b *StoreYieldBudget
	if _, err := b.HandoffBudget(context.Background(), board.Snapshot{}); !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("err = %v, want ErrUnavailable", err)
	}
}

// The provider is the missing half of the endpoint: with it wired, a yield
// request reaches a plan built from recorded state.
func TestStoreYieldBudgetSatisfiesTheEndpointsInterface(t *testing.T) {
	var provider BoardYieldBudget = NewStoreYieldBudget(&recordingBudgetStore{cohort: budgetCohort()})
	if provider == nil {
		t.Fatal("provider does not satisfy BoardYieldBudget")
	}
	if _, ok := any(&store.Store{}).(YieldBudgetStore); !ok {
		t.Fatal("the PostgreSQL store no longer satisfies YieldBudgetStore, so production wiring is dead")
	}
}
