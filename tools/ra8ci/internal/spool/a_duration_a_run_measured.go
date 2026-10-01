// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// measuredClockDisagreement is how far a reported duration may exceed the span
// of the stamps it arrives with before the two are a contradiction rather than
// two clocks disagreeing. The executor measures a duration from monotonic
// readings and stamps the same work with wall clock values, so the two differ
// by a small amount on every real run. Five seconds is the allowance this tree
// already fixes for exactly this comparison, in store.localClockDisagreement
// and in syncclient.uploadedClockDisagreement, so the freeze states the number
// those state rather than inventing a third one.
const measuredClockDisagreement = 5 * time.Second

// errUnmeasuredDuration names the one thing this rule refuses: a result handed
// to Finish stating that the attempt took longer than the window it ran in.
var errUnmeasuredDuration = errors.New("execution result states a duration no run measured")

// checkTheDurationWasMeasured holds the attempt's own duration to the window
// the attempt ran in, at the freeze.
//
// The freeze reads a great deal about each STEP: checkStepWindowsFitTheRecord
// holds every step's duration to the record's envelope, and the sweep holds
// each step's duration to its own stamps. Nothing on this host has ever read
// the one number the attempt states about itself. Result.Duration is what lands
// in local_runs.duration_ns and in every later account of how long the work
// took, and server.offlineInput takes an upload's own number as given rather
// than re-deriving it from the stamps.
//
// THE RULE is the store's, restated rather than imported
// (store.checkLocalRunDurations by way of checkLocalDurationFitsStamps): a
// duration may not be negative, and may not exceed the span of the stamps it
// arrives with by more than the clock allowance.
//
// The rule is one-sided on purpose, and that is the part worth keeping. A
// duration SHORTER than its span is ordinary: the stamps bracket the whole
// attempt while the duration may measure the child alone, and an attempt that
// came apart before the executor could measure anything carries no duration at
// all. Only a duration LONGER than the window that bounds it is a
// contradiction, because no clock reports more elapsed time than passed between
// the two readings it was measured from.
//
// This is the FREEZE, not the sweep, and the cut between them is the same one
// every door in this file's neighbourhood states: the sweep judges a record
// read back OFF DISK, where an older build's file or an edited one can say
// anything; this one judges what the executor in this process just handed over,
// before the bytes are written, so the outbox never holds the number in the
// first place. Refused at the sweep instead, the record is read, posted,
// refused with an opaque 400 and left in the outbox, and every unsynced record
// behind it waits on every pass.
//
// DELIBERATELY NOT judged against time.Now: the bound is read off the record
// alone, because this host's clock is the thing under suspicion and a freeze
// that compared against it would refuse honest records on a box whose clock had
// stepped. envelope_the_door_reads.go and step_windows_the_plane_measures.go
// state the same principle.
//
// DELIBERATELY NOT the per-step durations: checkStepWindowsFitTheRecord already
// holds each of those to the record, and stating the rule twice would report
// one record under two errors.
func checkTheDurationWasMeasured(result executor.Result) error {
	if result.Duration < 0 {
		return fmt.Errorf("%w: the attempt states a duration of %s",
			errUnmeasuredDuration, result.Duration)
	}
	if result.StartedAt.IsZero() || result.EndedAt.IsZero() {
		return nil
	}
	span := result.EndedAt.Sub(result.StartedAt)
	if result.Duration-span > measuredClockDisagreement {
		return fmt.Errorf("%w: the attempt states %s between stamps %s apart",
			errUnmeasuredDuration, result.Duration, span)
	}
	return nil
}
