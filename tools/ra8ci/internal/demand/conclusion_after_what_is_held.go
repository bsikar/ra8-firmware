// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import "time"

// conclusionStamp is when a stale conclusion can honestly say the job ended.
//
// The pass concludes demand the forge no longer knows about by writing a
// completed event stamped with its own clock: the plane never saw the job
// end, so the only moment it can name is the one it looked. That stamp has
// to sit after everything already on file about the same job, because
// Validate holds every event to the order a job can have happened in, and
// concluded() states the whole conclusion in one Event that has to pass it.
//
// One half of that was already here: a conclusion earlier than the queue time
// is carried forward to it. The other half, the start, was not, and the start
// is the stamp that can be ahead of this plane's clock. Every stamp on held
// demand comes from the forge's clock, through a delivery body; the pass's
// own `at` comes from the host running it. The two are not the same clock,
// and this is the package whose whole subject is hosts that miss deliveries.
// A host whose clock lags the forge by more than the job's run so far holds a
// StartedAt in its own future, and the conclusion it writes is stamped before
// the start it is concluding.
//
// What that costs is the unit of demand, permanently. Validate refuses the
// event as "completed before started", concluded() returns the error, decide
// counts a failure, and nothing is recorded, so the demand stays open. The
// next pass reads the same row, asks the forge the same question, gets the
// same missing answer and fails the same way, and every pass after it does
// too: the row can never leave `phase <> 'completed'`, it holds its place at
// the head of the oldest-first batch the pass reads, and with a full batch it
// crowds out demand behind it that checkOpenDemandFitsTheBatch can then only
// report as truncated. The operator reads one line naming a job and an
// invalid event, about a poll they cannot make succeed. That is the exact
// failure the reconciler exists to prevent, arriving through the reconciler.
//
// So the stamp is the latest of what the plane knows: its own observation,
// the queue time, and the start. Moving it later is the only direction that
// is honest here. The job did not end before it started, and the plane cannot
// say when it did end, so the earliest moment it can defend is the last one
// it has evidence for. ObservedAt is left alone: it is when the plane looked,
// which is true whatever the forge's clock says.
func conclusionStamp(held Event, at time.Time) time.Time {
	stamp := at.UTC()
	if stamp.Before(held.QueuedAt) {
		stamp = held.QueuedAt.UTC()
	}
	if !held.StartedAt.IsZero() && stamp.Before(held.StartedAt) {
		stamp = held.StartedAt.UTC()
	}
	return stamp
}
