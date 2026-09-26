package board

import "time"

// checkYieldRequestWindow holds a retained lease's yield-request stamp to the
// authority the lease actually had.
//
// YieldRequestedAt is not decoration: YieldSampleFor reads it as the moment the
// handoff clock started (yield_record.go), and every completed sample's latency
// is the distance from it to the neutral event. That latency is what
// ObserveYieldBudget quantiles into the ETA the next requester is shown, so a
// stamp from outside the lease does not merely look wrong in a row, it teaches
// the estimator a handoff duration no holder ever spent. PlanYield reads it the
// same way for the outstanding-request case.
//
// The reducer can only stamp it from inside the lease. All three writers set it
// to the Apply clock while the lease is live: enqueue when a higher-priority
// waiter arrives, acknowledge when one is already queued at install, and
// requestYield. The last two reach it only through a path that has already
// refused an expired lease, and grantNext stamps GrantedAt before any of them
// can run. So a retained stamp before the grant or at or past the expiry did
// not come from here, and the two ends are refused separately because they
// describe different accidents: a stamp before the grant is a request carried
// over from the previous holder of the same board, and one at or past expiry is
// a request attributed to a lease whose authority had already ended.
func checkYieldRequestWindow(lease *Lease) error {
	if lease == nil || lease.YieldRequestedAt.IsZero() {
		return nil
	}
	if lease.YieldRequestedAt.Before(lease.GrantedAt) {
		return &Error{Conflict, "retained lease carries a yield request from before its grant"}
	}
	// Strictly before expiry, matching the heartbeat window above: ExpiresAt
	// only ever moves later, so a stamp at or past it cannot be a request
	// this lease was able to answer.
	if !lease.YieldRequestedAt.Before(lease.ExpiresAt) {
		return &Error{Conflict, "retained lease carries a yield request from at or after its expiry"}
	}
	return nil
}

// yieldRequestWindow reports the span a retained yield request must fall in,
// used by the tests to state the rule once.
func yieldRequestWindow(lease Lease) (time.Time, time.Time) {
	return lease.GrantedAt, lease.ExpiresAt
}
