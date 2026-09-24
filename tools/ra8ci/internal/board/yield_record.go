package board

import "time"

// Recorded exclusion reasons. A censored sample is retained as evidence that a
// yield was requested and did not complete; it is never measured as a latency.
// The audit event carries the detail, so these stay short and stable enough to
// group a cohort by failure mode.
const (
	YieldExcludedNoReceipt  = "released without a neutral receipt"
	YieldExcludedExpired    = "lease expired before neutral"
	YieldExcludedRecovery   = "recovery required before neutral"
	YieldExcludedQuarantine = "board quarantined before neutral"
	YieldExcludedWithdrawn  = "yield request withdrawn before neutral"
)

// YieldSampleFor derives the yield sample a committed transition leaves
// behind, if any.
//
// before is the snapshot the command was applied to and events are the events
// that command produced, which is exactly what the store commits in one
// transaction, so the sample is written by the same transaction that made it
// true. shownTarget is the handoff estimate the requester was shown; a
// completed handoff that ran past it is flagged as a safety overrun and stays
// in the history. Pass zero when no estimate was shown.
//
// The second return is false when the transition measures nothing: no lease,
// no outstanding yield request, or no event that ended the handoff either way.
// A sample is refused rather than corrected when it would contradict itself,
// for instance a board reaching neutral before the yield that asked for it.
func YieldSampleFor(before Snapshot, events []Event, cohort YieldCohort, shownTarget time.Duration) (YieldSample, bool, error) {
	if err := ValidateYieldCohort(cohort); err != nil {
		return YieldSample{}, false, err
	}
	if shownTarget < 0 || shownTarget > MaxHandoffBound {
		return YieldSample{}, false, &Error{InvalidArgument, "shown handoff target is out of range"}
	}
	lease := before.Lease
	if lease == nil || lease.YieldRequestedAt.IsZero() {
		return YieldSample{}, false, nil
	}

	sample := YieldSample{
		Cohort:      cohort,
		LeaseID:     lease.ID,
		WaiterID:    lease.WaiterID,
		RequestedAt: lease.YieldRequestedAt,
	}
	terminal, found := terminalYieldEvent(lease.ID, events)
	if !found {
		return YieldSample{}, false, nil
	}
	switch terminal.Kind {
	case LeaseReleased:
		sample.NeutralAt = terminal.At
	case LeaseExpired:
		sample.ExclusionReason = YieldExcludedExpired
	case RecoveryNeeded:
		// A release with no neutral receipt is the interesting case: the
		// holder said it was done and could not prove the board was safe.
		// It is named apart from any other route into recovery.
		sample.ExclusionReason = YieldExcludedRecovery
		if terminal.Actor != "server" {
			sample.ExclusionReason = YieldExcludedNoReceipt
		}
	case BoardQuarantined:
		sample.ExclusionReason = YieldExcludedQuarantine
	case YieldCleared:
		sample.ExclusionReason = YieldExcludedWithdrawn
	}
	if err := validateYieldSample(sample, terminal.At); err != nil {
		return YieldSample{}, false, err
	}
	if sample.Completed() && shownTarget > 0 && sample.Latency() > shownTarget {
		sample.SafetyOverrun = true
	}
	return sample, true, nil
}

// terminalYieldEvent returns the first event for this lease that ends the
// handoff one way or the other. Only the first counts: a transition that
// releases a lease and immediately grants the next one must not let the new
// grant's events describe the old lease's handoff.
func terminalYieldEvent(leaseID string, events []Event) (Event, bool) {
	for _, candidate := range events {
		if candidate.LeaseID != leaseID {
			continue
		}
		switch candidate.Kind {
		case LeaseReleased, LeaseExpired, RecoveryNeeded, BoardQuarantined, YieldCleared:
			return candidate, true
		}
	}
	return Event{}, false
}
