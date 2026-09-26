// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import "fmt"

// checkRecordedStepDuration holds the duration a recorded step states to the
// pair of stamps it is stated with.
//
// Every other writer of task_steps already does this. A dispatched step's
// receipt is held to its own stamps by protocol.checkReportedDurations before
// the row is written, a HIL step by the inline rule in RecordHILCompletion
// (board_hil_completion.go), and a local upload by checkLocalRunDurations.
// RecordStep, the store's own exported step-recording API, judged the same
// number only for being non-negative, so a step reporting an hour of work
// between stamps a millisecond apart was written to durable history.
//
// The number matters after the write, not just in the row. task_steps.duration_ns
// is what every later account of how long the step took is read from, and
// RecordHILObservation copies it straight out of the step row into
// hil_observations.duration_ns, which is the fleet's timing evidence for that
// workload. RecordStep takes the step's phase from its caller, so an
// hil_observe step can be recorded through this door and carried into that
// history by the next observation. RecordHILObservation bounds what it copies
// (positive, under an hour) but never compares it with the step's own stamps,
// which it reads in the same query.
//
// The rule is one-sided, exactly as its three siblings are. A duration SHORTER
// than its span is ordinary: the stamps bracket the whole step while the
// duration may measure the child alone, and a step with no measurement at all
// reports zero. Only a duration longer than the window it was measured in is a
// contradiction, because no clock reports more elapsed time than passed between
// the two readings that bound it.
//
// The allowance is localClockDisagreement, the five seconds this package
// already fixes for this comparison, rather than a second number.
func checkRecordedStepDuration(in StepInput) error {
	return checkLocalDurationFitsStamps(fmt.Sprintf("step %s", in.Key), in.DurationNS, in.StartedAt, in.EndedAt)
}
