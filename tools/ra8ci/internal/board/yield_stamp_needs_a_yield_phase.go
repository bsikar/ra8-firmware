package board

// A yield request stamp belongs only to a phase that asked for one.
//
// Validate already holds one direction of this: a board in YieldRequested or
// Draining without a YieldRequestedAt is refused, because the phase claims a
// handoff is running and nothing says when it started. The other direction was
// never asked, and it is the one a stale row actually takes: a lease sitting in
// GrantPending or Active while still carrying the stamp of a request that is no
// longer outstanding.
//
// The reducer cannot produce it. All three writers set the stamp and the phase
// together while the lease is live (enqueue when a higher-priority waiter
// arrives, acknowledge when one is already queued at install, requestYield),
// and the one clearer clears them together too: cancel drops the stamp, the
// target and the cohort in the same step it returns the board to Active. So a
// live non-yield phase holding a stamp did not come from here, which is exactly
// why it goes unread rather than being caught downstream.
//
// It is not caught downstream because neither reader looks at the phase.
// YieldSampleFor keys the whole measurement off the stamp being nonzero, so the
// next terminal event files a completed sample whose latency is the distance
// from a request nobody made; that latency is not a stray row but an input to
// the nearest-rank quantile ObserveYieldBudget computes over a deliberately
// small sample floor, so one phantom handoff moves the ETA every later
// requester on this cohort is shown. PlanYield reads it the same way and
// answers sooner: it reports the handoff Outstanding, anchors RequestedAt to
// the stale stamp instead of now, pins the target through promised(), and can
// return a plan already Overdue for a board that was never asked to yield.
//
// The recovery phases are deliberately outside the rule. RecoveryRequired,
// Recovering and Quarantined retain the lease as evidence, and a yield that was
// outstanding when the board went that way is part of the evidence: it is what
// lets YieldSampleFor record the censored sample naming recovery or quarantine
// as the reason the handoff never completed. Ready retains no lease at all.
//
// Refused rather than cleared, in the same words as the window rules next to
// it: the stamp is the start of a measurement, and a snapshot whose phase and
// stamp disagree has one of the two wrong. Clearing the stamp would pick the
// phase as the truth without saying so, and the sample that went missing would
// be the only trace left.
func checkYieldStampNeedsAYieldPhase(phase Phase, lease *Lease) error {
	if lease == nil || lease.YieldRequestedAt.IsZero() {
		return nil
	}
	if phase != GrantPending && phase != Active {
		return nil
	}
	return &Error{Conflict, "retained lease carries a yield request in phase " + string(phase) + ", which asked for none"}
}
