// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The third step of the unclaimed sequence, release_lease.
//
// The step's honest shape is a proof, not a release, and the reasoning is
// worth stating where the code is. A bench lease is acquired at StartAttempt
// by an agent that already holds a claimed runner; leases hang off attempts,
// attempts key on task_id, and runner_vms keys on the forge's job identity.
// A reservation nobody claimed never reached that code, so it holds no lease,
// and there is no column that would let this step find one if it did.
//
// Two things follow. The step asks the bench under the identities the plane
// could have written into a holder id and requires the answer to be none: the
// reaper runs precisely when the plane's picture of a reservation is already
// wrong once, so "it cannot have a lease" is an argument, not an observation.
// And when the bench does name one, the step stops instead of ending it.
// board.Release needs a neutral receipt the trusted server verifies against
// the board's current generation and fixture profile, which this pass cannot
// manufacture, and a lease matched only by name could belong to live work on
// real hardware. Stopping leaves the reservation in the reaper's queue with
// the contradiction visible in the pass report; ending somebody's board on a
// name match would not be recoverable.

// ErrUnclaimedLeaseHeld means the bench holds a live lease under an identity
// belonging to a reservation nobody claimed. It is a contradiction between
// the ledger and the bench, so the sequence stops and the row stays queued.
var ErrUnclaimedLeaseHeld = errors.New("bench holds a live lease for an unclaimed reservation")

// BoardLeaseLookup is the bench read behind the step. *store.Store satisfies
// it; the seam exists so the guard can be tested without a database and so a
// caller must opt in rather than get the whole store by accident.
type BoardLeaseLookup interface {
	ListLiveBoardLeasesByHolder(context.Context, string) ([]store.LiveBoardLease, error)
}

var _ BoardLeaseLookup = (*store.Store)(nil)

// UnclaimedLeaseGuard is the concrete LeaseReleaser the revoker had no
// implementation for. Before this, a wired revoker needed a bench seam that
// did not exist, so the sequence could not be assembled at all.
type UnclaimedLeaseGuard struct {
	leases BoardLeaseLookup
}

var _ LeaseReleaser = (*UnclaimedLeaseGuard)(nil)

// NewUnclaimedLeaseGuard refuses a guard with no bench to ask. A guard that
// cannot ask would answer "no lease held" to everything, which is the one
// answer that must be earned.
func NewUnclaimedLeaseGuard(leases BoardLeaseLookup) (*UnclaimedLeaseGuard, error) {
	if leases == nil {
		return nil, errors.New("unclaimed lease guard needs a bench")
	}
	return &UnclaimedLeaseGuard{leases: leases}, nil
}

// ReleaseReservationLease performs the step. A reservation with no live lease
// under any of its identities is the ordinary case and a success.
func (g *UnclaimedLeaseGuard) ReleaseReservationLease(ctx context.Context, vm store.RunnerVM) error {
	if g == nil || g.leases == nil || ctx == nil {
		return errors.New("invalid unclaimed lease guard")
	}
	holders := store.UnclaimedLeaseHolders(vm)
	if len(holders) == 0 {
		return fmt.Errorf("%w: reservation carries no identity to ask the bench about", store.ErrInvalid)
	}
	for _, holder := range holders {
		live, err := g.leases.ListLiveBoardLeasesByHolder(ctx, holder)
		if err != nil {
			return fmt.Errorf("read bench leases for %s: %w", holder, err)
		}
		if len(live) == 0 {
			continue
		}
		lease := live[0]
		return fmt.Errorf("%w: reservation %s, holder %s, lease %s on board %s (%s, %s priority)",
			ErrUnclaimedLeaseHeld, vm.ID, holder, lease.ID, lease.BoardID, lease.State, lease.Priority)
	}
	return nil
}
