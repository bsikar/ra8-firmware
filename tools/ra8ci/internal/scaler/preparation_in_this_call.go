// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"fmt"
	"time"
)

// preparationClockSkew is the allowance between the stamp a bootstrap receipt
// carries and the stamps this host takes around the call that produced it. It
// is the one second bootstrapRunning already allows when it holds PreparedAt
// against now, and the same second the ledger allows when it holds the same
// field against the database clock (store/runner_vms.go), so the rule states
// the allowance those two doors already state rather than inventing a third.
const preparationClockSkew = time.Second

// checkPreparationHappenedInThisCall holds a bootstrap receipt's preparation
// stamp to the call that returned it. The handler brackets Prepare with two
// readings of its own clock and files all three numbers together, as
// bootstrap_started_at, bootstrap_completed_at and prepared_at on one audit
// row (store/runner_vms.go, RecordRunnerVMBootstrapEvidence). The bracket is
// there to say when this guest was made ready.
//
// Nothing tied the stamp to the bracket. bootstrapRunning judges PreparedAt
// against now: not more than a second ahead, not more than five minutes
// behind. The ledger repeats exactly that judgement, and separately holds
// StartedAt at or before CompletedAt and the whole window under five minutes.
// So the two halves of the row were each judged on their own and never
// against each other, and a receipt could state a preparation that finished
// before the handler asked for one.
//
// A stamp before the call is a receipt from an earlier call. Everything else
// on it survives that: Prepare is required to reconcile idempotently by
// reservation ID after an ambiguous response (the Bootstrapper contract), so
// a second attempt on the same reservation carries the same ReservationID,
// the same VMID and the same CommitSHA, and those are the fields the identity
// check compares. A five-minute window is many bootstrap attempts wide, and
// the retry path is exactly where a stale receipt comes from, so the one
// field that could tell a fresh preparation from a replayed one was the one
// field nothing read.
//
// What the row is for decides how much that costs. An operator reading it
// asks whether THIS guest was made ready with THIS readiness digest, THIS
// runner binary and THIS JIT configuration before the forge was allowed to
// put a job on it, and the answer is the row's four digests read beside its
// stamps. A prepared_at outside the bracket means the digests describe a
// preparation that happened before the call being recorded, and the row still
// reads as though the guest was made ready during it. That is the one reading
// the evidence exists to support.
//
// The rule refuses only a stamp outside the bracket, with the clock allowance
// on both sides. It says nothing about how long the bootstrap took or how far
// into the window the preparation landed: a preparation finishing at either
// edge of its own call is ordinary, and the ledger already bounds the window
// itself.
func checkPreparationHappenedInThisCall(reservationID string, startedAt, completedAt, preparedAt time.Time) error {
	if startedAt.IsZero() || completedAt.IsZero() {
		return fmt.Errorf("bootstrap of reservation %s was not bracketed by this host's clock", reservationID)
	}
	if completedAt.Before(startedAt) {
		return fmt.Errorf("bootstrap of reservation %s reports returning before it was asked for", reservationID)
	}
	if preparedAt.Before(startedAt.Add(-preparationClockSkew)) {
		return fmt.Errorf("bootstrap receipt for reservation %s was prepared %s before the bootstrap was asked for",
			reservationID, startedAt.Sub(preparedAt))
	}
	if preparedAt.After(completedAt.Add(preparationClockSkew)) {
		return fmt.Errorf("bootstrap receipt for reservation %s claims a preparation %s after the bootstrap returned",
			reservationID, preparedAt.Sub(completedAt))
	}
	return nil
}
