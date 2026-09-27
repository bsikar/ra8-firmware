// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Holding a recorded yield sample's request stamp to the transition that ends
// the handoff.
//
// The read already refuses a row this plane's clocks cannot account for
// (sample_fits_the_clock.go): both stamps are held to now widened by
// board.MaxClockOffset, because requested_at is the ORDER BY key of that read
// and a row stamped ahead of the reading host sits at the head of every page
// of its cohort. The cutoff bounds only the old side, and a future stamp is
// always inside a window measured backwards from now, so such a row does not
// age out; it holds the head until the clock catches up to it, and because the
// read refuses the page rather than skipping the row, the whole cohort's
// history stays unreadable for as long as that takes. For a host set months
// ahead, that is months of ETAs falling back to declared bounds.
//
// Nothing stopped that row being written. The writer's own checks are about
// the row's shape (measured or censored, never both; identity present), and
// the only clock judgement on the way in is board.validateYieldSample, which
// returns as soon as a sample carries an exclusion reason. So:
//
//   - A MEASURED row is already covered, by arithmetic rather than by a rule
//     about the request stamp. The neutral time must be after the request and
//     no further ahead of the terminal event than MaxClockOffset, so a request
//     stamp beyond that horizon leaves no neutral time that can satisfy both.
//   - A CENSORED row is not covered at all. Its request stamp is copied from
//     the lease and written untouched, and a lease carries the stamp of
//     whichever plane host recorded the yield REQUEST, which is not the host
//     committing the end of the handoff. Two hosts, two clocks, and the plane
//     already names what it tolerates between them.
//
// So the rule is applied on the way in, to the stamp that survives every
// exclusion, against the transition's own time. It is the same horizon the
// read and the estimator use, deliberately: a row the read would refuse must
// not reach durable history in the first place, because at the read it is
// already too late to do anything but refuse the cohort.
//
// Refusing here fails the transition, which is the stance this recorder
// already takes for a sample it cannot file (a row neither measured nor
// censored, a row missing its identity) and the stance board.validateYieldSample
// takes for a neutral time in the future. A handoff whose own request stamp
// this host cannot account for is that same finding arriving one stamp earlier.

// endOfHandoff returns the time of the first event that ends this lease's
// handoff one way or the other.
//
// It re-derives what board.YieldSampleFor derived to build the sample, which
// is the derive-twice shape this seam uses elsewhere: the store cannot see
// board's own choice, and a stamp taken from anywhere else in the transition
// would judge the request against an event that did not end the handoff. Only
// the first counts, for the reason board gives: a transition that releases one
// lease and grants the next must not let the new grant describe the old
// lease's handoff.
func endOfHandoff(leaseID string, events []board.Event) (time.Time, bool) {
	for _, candidate := range events {
		if candidate.LeaseID != leaseID {
			continue
		}
		switch candidate.Kind {
		case board.LeaseReleased, board.LeaseExpired, board.RecoveryNeeded,
			board.BoardQuarantined, board.YieldCleared:
			return candidate.At, true
		}
	}
	return time.Time{}, false
}

// yieldRequestStampFitsTheTransition refuses a sample whose request stamp runs
// further ahead of the transition ending the handoff than the plane's own
// clock tolerance allows.
func yieldRequestStampFitsTheTransition(row yieldSampleRow, events []board.Event) error {
	ended, found := endOfHandoff(row.LeaseID, events)
	if !found {
		// Unreachable through the recorder, which files nothing without a
		// terminal event. Named rather than assumed, because the alternative
		// is judging a request stamp against the zero time and refusing every
		// row ever written.
		return fmt.Errorf("%w: yield sample %s has no transition to be judged against", ErrConflict, row.LeaseID)
	}
	if ended.IsZero() {
		return fmt.Errorf("%w: yield sample %s ends on an unstamped event", ErrConflict, row.LeaseID)
	}
	// The same horizon the read and the estimator use, so the three cannot
	// disagree about which stamps are believable.
	if row.RequestedAt.After(ended.Add(board.MaxClockOffset)) {
		return fmt.Errorf("%w: yield sample %s is requested after the transition that ends it", ErrConflict, row.LeaseID)
	}
	return nil
}
