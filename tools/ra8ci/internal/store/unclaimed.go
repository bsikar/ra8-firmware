// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"
	"time"
)

// A just-in-time runner credential is single use and is minted before the
// guest that will consume it exists. Between the mint and the job starting,
// nothing in the forge ever says the credential was wasted: a scale-set
// listener got that for free because it held the assignment itself. This
// plane holds the assignment instead, so it has to state the deadline.
//
// The rules live here rather than inside the write sites so the reaper's
// candidate query, the reservation write and the guards all read the same
// predicate, the same way the state machines in transitions.go are stated
// once instead of once per `AND state='...'` clause.

const (
	// MinUnclaimedLease is the shortest deadline a reservation may carry.
	// A lease shorter than this races the clone it is waiting for.
	MinUnclaimedLease = time.Minute
	// MaxUnclaimedLease bounds how long a minted credential may sit
	// unused. A credential outliving this is an operator decision, not a
	// default.
	MaxUnclaimedLease = 24 * time.Hour
	// DefaultUnclaimedLease is what a caller that has no opinion gets.
	DefaultUnclaimedLease = 30 * time.Minute
)

// ValidUnclaimedLease reports whether a lease duration is inside the bounds.
func ValidUnclaimedLease(lease time.Duration) bool {
	return lease >= MinUnclaimedLease && lease <= MaxUnclaimedLease
}

// UnclaimedDeadline is the deadline a reservation made at now carries. It is
// computed once, at reservation, and never recomputed: extending a deadline
// on replay would let a repeated delivery keep a dead credential alive.
func UnclaimedDeadline(now time.Time, lease time.Duration) (time.Time, error) {
	if now.IsZero() {
		return time.Time{}, fmt.Errorf("%w: unclaimed deadline needs a clock", ErrInvalid)
	}
	if !ValidUnclaimedLease(lease) {
		return time.Time{}, fmt.Errorf("%w: unclaimed lease %s outside [%s,%s]",
			ErrInvalid, lease, MinUnclaimedLease, MaxUnclaimedLease)
	}
	return now.Add(lease).UTC(), nil
}

// ValidUnclaimedDeadline checks a deadline supplied by a caller against the
// same bounds. The database CHECK only knows the deadline is after the row's
// creation; this is what stops a one second lease or a lease into next year.
func ValidUnclaimedDeadline(now, deadline time.Time) error {
	if now.IsZero() || deadline.IsZero() {
		return fmt.Errorf("%w: unclaimed deadline is unset", ErrInvalid)
	}
	lease := deadline.Sub(now)
	if !ValidUnclaimedLease(lease) {
		return fmt.Errorf("%w: unclaimed deadline is %s away, want [%s,%s]",
			ErrInvalid, lease, MinUnclaimedLease, MaxUnclaimedLease)
	}
	return nil
}

// unclaimedCandidate is the reaper's candidate set, written once. The Go
// predicate below and every SQL site that selects candidates are both written
// over it, so the query and the guard after the lock cannot drift the way the
// reaper's attempt state list did before AttemptReapable.
const unclaimedCandidate = `claimed_at IS NULL AND state <> 'released' AND unclaimed_deadline <= $%d`

// UnclaimedExpired reports whether a reservation's deadline has passed with no
// job ever taking it. A released reservation has nothing left to revoke, and a
// claimed one is out of reach for good: whatever happens to the job afterwards
// is the dispatch reaper's business, not this one's.
func UnclaimedExpired(vm RunnerVM, now time.Time) bool {
	return vm.ClaimedAt == nil && vm.State != "released" &&
		!vm.UnclaimedDeadline.IsZero() && !vm.UnclaimedDeadline.After(now)
}

// UnclaimedReservation reports whether the reaper still has an interest in
// this reservation at all, deadline aside. It is the half of the predicate
// that does not move with the clock, which is what a caller holding a row
// under a lock wants to re-check.
func UnclaimedReservation(vm RunnerVM) bool {
	return vm.ClaimedAt == nil && vm.State != "released"
}
