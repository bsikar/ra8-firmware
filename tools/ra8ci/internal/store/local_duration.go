// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"
	"math"
	"time"
)

// localClockDisagreement is how far a reported duration may exceed the span of
// the stamps it arrives with before the two are a contradiction rather than two
// clocks disagreeing. The executor measures a duration from a pair of monotonic
// readings and stamps the same work with their wall clock values, so the two
// differ by a small amount on every real run. Five seconds is the allowance this
// tree already fixes for exactly this comparison, in board_hil_completion.go
// where HIL step evidence is judged and in protocol/receipt_duration.go where an
// agent receipt is, so the local path states the number those state rather than
// inventing a third one.
const localClockDisagreement = 5 * time.Second

// checkLocalRunDurations holds every duration a local upload states to the pair
// of stamps it is stated with.
//
// A local run is client JSON reported about work that ran on the caller's own
// host, so the stamps and the duration arrive as independent numbers and nothing
// before this compared them: validateLocalRun judged StartedAt against
// FinishedAt and every duration only for being non-negative. The rest of the
// tree does compare them. An agent receipt is held to its stamps by
// protocol.checkReportedDurations, a HIL step by the inline rule in
// RecordHILCompletion, and a dispatched step by its receipt's total in
// dispatch.go. The offline sync surface was the one door where a run could
// report an hour of work between stamps a millisecond apart and have that hour
// committed to durable history.
//
// The duration is the number that lands in local_runs.duration_ns and in every
// later account of how long the work took, and this path never re-derives it:
// server/offline.go computes a duration from the stamps only for entries it
// builds itself, while an upload's own number is taken as given.
//
// The rule is one-sided on purpose. A duration SHORTER than its span is
// ordinary: the stamps bracket the whole attempt while the duration may measure
// the child alone, and an incomplete_evidence upload may carry no measurement at
// all. Only a duration longer than the window it was measured in is a
// contradiction, because no clock reports more elapsed time than passed between
// the two readings that bound it.
func checkLocalRunDurations(in LocalRunInput) error {
	if err := checkLocalDurationFitsStamps("local run", in.DurationNS, in.StartedAt, in.FinishedAt); err != nil {
		return err
	}
	for i, step := range in.Steps {
		subject := fmt.Sprintf("local step %d", i)
		if err := checkLocalDurationFitsStamps(subject, step.DurationNS, step.StartedAt, step.EndedAt); err != nil {
			return err
		}
	}
	return nil
}

// checkLocalDurationFitsStamps refuses a duration longer than the span it names,
// and refuses a span too wide to measure at all. time.Time.Sub saturates at
// MaxInt64 rather than reporting an overflow, so a pair of stamps more than
// about 292 years apart yields the largest representable span and would admit
// any duration whatsoever. Callers reach this only after validateLocalRun has
// held the later stamp at or after the earlier one, so a saturated result means
// an absurd span and never a reversed one, and the subtraction below cannot
// overflow: both sides are non-negative.
func checkLocalDurationFitsStamps(subject string, durationNS int64, started, ended time.Time) error {
	span := ended.Sub(started)
	if span == time.Duration(math.MaxInt64) {
		return fmt.Errorf("%w: %s spans longer than a clock can measure", ErrInvalid, subject)
	}
	if durationNS-span.Nanoseconds() > localClockDisagreement.Nanoseconds() {
		return fmt.Errorf("%w: %s reports %dns between stamps %dns apart",
			ErrInvalid, subject, durationNS, span.Nanoseconds())
	}
	return nil
}
