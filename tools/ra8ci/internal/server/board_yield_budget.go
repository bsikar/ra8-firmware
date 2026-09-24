package server

import (
	"context"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The production handoff budget.
//
// POST /v1/boards/{id}/yield has answered 503 since it landed, because
// BoardYieldBudget had only a test fake behind it. The three inputs an
// estimate needs now all exist: the cohort and the task's declared bounds are
// derived from the work the board holds (store.HeldYieldWork), and the
// comparable history is read back from the samples the board transaction
// writes (store.YieldHistory). This joins them.
//
// It stays an adapter rather than a method on the store because the estimate
// belongs to neither half: the store owns what is on disk, board owns the
// judgement, and the server owns the door. A store returning a server type
// would invert that.

// YieldBudgetStore is the store half of a handoff budget.
type YieldBudgetStore interface {
	HeldYieldWork(context.Context, board.Snapshot) (board.YieldCohort, board.DeclaredHandoffBounds, error)
	YieldHistory(context.Context, board.YieldCohort, time.Time) ([]board.YieldSample, error)
}

// StoreYieldBudget answers a yield request's budget from recorded state.
type StoreYieldBudget struct {
	store YieldBudgetStore
	now   func() time.Time
}

// NewStoreYieldBudget wires a budget provider to a store. A nil store yields
// a nil provider, which leaves the yield door closed rather than open with
// the estimate skipped, the same shape as a missing neutral verifier.
func NewStoreYieldBudget(st YieldBudgetStore) *StoreYieldBudget {
	if st == nil {
		return nil
	}
	return &StoreYieldBudget{store: st, now: func() time.Time { return time.Now().UTC() }}
}

// HandoffBudget derives the cohort and bounds for the work this board holds,
// then reads that cohort's history.
//
// The order matters and is the property: the cohort comes first and the
// history is read for THAT cohort. Reading history first would mean choosing
// a bucket before knowing what work is in front of it, which is how an
// estimate ends up resting on a fixture or an image the board is no longer
// running.
func (b *StoreYieldBudget) HandoffBudget(ctx context.Context, snapshot board.Snapshot) (HandoffBudget, error) {
	if b == nil || b.store == nil {
		return HandoffBudget{}, fmt.Errorf("%w: yield budget has no store", store.ErrUnavailable)
	}
	cohort, bounds, err := b.store.HeldYieldWork(ctx, snapshot)
	if err != nil {
		return HandoffBudget{}, err
	}
	samples, err := b.store.YieldHistory(ctx, cohort, b.clock())
	if err != nil {
		return HandoffBudget{}, err
	}
	return HandoffBudget{Cohort: cohort, Bounds: bounds, Samples: samples}, nil
}

func (b *StoreYieldBudget) clock() time.Time {
	if b.now == nil {
		return time.Now().UTC()
	}
	return b.now()
}
