// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Building the unclaimed reaper out of the handler's own configuration, so a
// deployment cannot end up with a reaper pointed at a different scale set, a
// different actor or a different batch size than the handler that mints the
// credentials it is reaping. Everything here is derived; nothing is passed a
// second time.
//
// The revoker stays injected. Its four steps reach the forge, the hypervisor
// and the bench, and the handler holds only some of those: binding it here
// would make the reaper untestable without a full handler and would hide
// which system each step actually talks to.

// unclaimedReapBatch is one pass's ceiling when the handler has no reconcile
// batch configured. Expired reservations are cheap to leave for the next
// pass and expensive to rush: nothing is lost by taking them in bites.
const unclaimedReapBatch = 50

// UnclaimedReaper builds a reaper over this handler's ledger and scale set.
// The handler is the thing that knows which scale set it serves, so it is
// the thing that should say so.
func (h *Handler) UnclaimedReaper(revoker UnclaimedRevoker) (*UnclaimedReaper, error) {
	if h == nil || h.ledger == nil {
		return nil, errors.New("unclaimed reaper needs a wired handler")
	}
	return NewUnclaimedReaper(UnclaimedReaperConfig{
		ScaleSetID: h.config.ScaleSetID,
		BatchSize:  h.unclaimedReapBatch(),
	}, h.ledger, revoker)
}

// unclaimedReapBatch reuses the reconcile batch when one is configured. A
// deployment that has already said how much work it wants in one pass has
// said it for this pass too.
func (h *Handler) unclaimedReapBatch() int {
	if h.config.MaxReconcileBatch > 0 && h.config.MaxReconcileBatch <= 1000 {
		return h.config.MaxReconcileBatch
	}
	return unclaimedReapBatch
}

// ReapUnclaimed runs one pass and reports what it did. It is separate from
// Reconcile on purpose: reconciliation resolves operations this plane already started
// and cannot safely be skipped, while this pass revokes credentials nobody
// took and is safe to run on its own schedule, or not at all for a while.
//
// The report comes back whether or not the pass was complete, because the
// counts are what actually happened either way.
func (h *Handler) ReapUnclaimed(ctx context.Context, revoker UnclaimedRevoker) (UnclaimedReport, error) {
	reaper, err := h.UnclaimedReaper(revoker)
	if err != nil {
		return UnclaimedReport{}, err
	}
	return reaper.Reap(ctx)
}

// ExpiredUnclaimed is the handler's own view of its queue, for a caller that
// wants to see what the next pass would take without taking it. It is the
// same read the reaper performs, so an operator inspecting the queue and the
// reaper acting on it cannot see different rows.
func (h *Handler) ExpiredUnclaimed(ctx context.Context, now time.Time) ([]store.RunnerVM, error) {
	if h == nil || h.ledger == nil {
		return nil, errors.New("unclaimed queue needs a wired handler")
	}
	if now.IsZero() {
		return nil, errors.New("unclaimed queue needs a clock")
	}
	return h.ledger.ListExpiredUnclaimedRunnerVMs(ctx, h.config.ScaleSetID, now.UTC(), h.unclaimedReapBatch())
}
