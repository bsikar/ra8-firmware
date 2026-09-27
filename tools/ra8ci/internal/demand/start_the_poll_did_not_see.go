// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import "time"

// startTheJobAlreadyHad answers what a reconciled event's start should be
// when the poll's snapshot does not carry one.
//
// observed() takes the snapshot's StartedAt verbatim, so a snapshot with no
// start erases a start the plane already has evidence for. The line directly
// below it does not work that way: RunnerName is only overwritten when the
// snapshot names one, because a poll that did not read a field has not
// learned the field is empty. The start was the same kind of claim and was
// being treated as a reading.
//
// What it costs is the unit of demand, permanently, and by the same route
// conclusionStamp exists to close. Validate refuses a non-queued event with
// no start, so a job held in_progress with a forge-clock start on file, whose
// snapshot comes back completed with started_at absent, fails in observed();
// decide returns the error, Pass counts a failure and records nothing, and
// the demand stays open. The next pass reads the same row, asks the same
// question, gets the same answer and fails the same way. The row keeps its
// place at the head of the oldest-first batch ListOpen serves, so with a full
// batch it also crowds out demand behind it that
// checkOpenDemandFitsTheBatch can then only report as truncated. A dropped
// completion is exactly what this pass exists to turn into a late run, and
// this is the shape of dropped completion it could never settle.
//
// A snapshot is not obliged to carry a start for the plane to be wrong about
// this: JobSource is an interface, the jobs API answers a cancelled job with
// no started_at at all, and the package's own stance is that an adapter
// growing or dropping a field must not quietly emit demand the rest of the
// plane cannot place.
//
// The rule is one-sided and phase-aware. A snapshot that DOES carry a start
// wins outright, even an earlier one: the forge owns its own stamps and a
// later reading of them is a correction, not a loss. And a snapshot that has
// regressed to queued gets no start carried into it, because a queued event
// holding a start is what checkStampsMatchThePhase refuses; today such a
// snapshot is judged not to supersede what is held and the pass leaves the
// row alone, which is the right answer and stays the answer.
func startTheJobAlreadyHad(held Event, snapshot JobSnapshot) time.Time {
	if !snapshot.StartedAt.IsZero() {
		return snapshot.StartedAt
	}
	if snapshot.Phase == PhaseQueued {
		return time.Time{}
	}
	return held.StartedAt
}
