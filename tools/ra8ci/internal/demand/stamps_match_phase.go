// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import "fmt"

// checkStampsMatchThePhase holds the two stamps a job earns as it runs to the
// phase the event says it reached: a queued job has neither a start nor a
// completion, and a running job has no completion.
//
// Validate already states exactly this rule for the conclusion, which is the
// same claim in a different column: a conclusion before completion is
// refused, because a job that has not finished cannot say how it finished. A
// job that has not finished cannot say WHEN it finished either, and that arm
// went unstated. Both doors into this package reach it. NormalizeWorkflowJob
// phase-guards started_at and reads completed_at whatever the action says, so
// a queued delivery carrying a completion keeps it; the reconciliation pass
// copies snapshot.StartedAt and snapshot.CompletedAt onto the event for
// whichever phase the forge answered with, so a poll answering queued with a
// start time writes a queued row that says the job is already running.
//
// The cost is not a spare column. A queue wait downstream is StartedAt minus
// QueuedAt and a run is CompletedAt minus StartedAt, so a row holding a
// completion over a zero start reads as a run measured from the zero time.
// Phase is also what Supersedes ranks on, so such a row is not corrected by
// anything until the genuine later delivery arrives, and if that delivery is
// one of the dropped ones this package exists to cover, it never does.
//
// The rule is one-sided. A phase MISSING the stamp it needs is Validate's own
// question, asked before this one: a non-queued event with no start and a
// completed event with no completion are refused there. This side only
// refuses a stamp that arrives ahead of the phase that earns it.
func checkStampsMatchThePhase(e Event) error {
	if e.Phase == PhaseQueued && !e.StartedAt.IsZero() {
		return fmt.Errorf("%w: start time before the job left the queue", ErrInvalid)
	}
	if e.Phase != PhaseCompleted && !e.CompletedAt.IsZero() {
		return fmt.Errorf("%w: completion time before completion", ErrInvalid)
	}
	return nil
}
