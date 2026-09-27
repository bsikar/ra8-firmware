// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
)

// errStepOutsideTheRecord names the one thing this rule refuses: a terminal
// record carrying a step whose own window the record's envelope cannot hold.
var errStepOutsideTheRecord = errors.New("terminal record states a step outside the run it reports")

// checkStepWindowsFitTheRecord holds every step inside a terminal record to
// the envelope the record itself states.
//
// checkTerminalRecordCarriesItsRun judges the record: a result is present and
// the record's own stamps, and the executor window inside them, are in order.
// It stops there. The steps the result carries were never looked at by
// anything in this package, and they are uploaded verbatim: SyncPending
// marshals the record as it stands, and offlineInput copies each step's
// StartedAt, EndedAt and Duration straight into store.LocalStepInput.
//
// The ingest end does look, in checkLocalStepIsStated, and refuses a step that
// states no start or no end, one that ended before it started, one stamped
// outside the record's envelope, and a duration that is negative or longer
// than that envelope. Those four refusals depend on nothing but the record, so
// a record carrying such a step is refused on every upload, from every host,
// forever. That is the shape this door already exists for: SyncPending returns
// on the first response that is not 200, so one unusable file at the front of
// the outbox stops a disconnected host from delivering any of the evidence it
// kept, and the operator reads back "upload local <id> returned HTTP 400" with
// no step named.
//
// Refused here it costs the sweep the same pass, but the pass names the file,
// the step and what is wrong with it, which is the whole difference this
// package has already chosen twice. A record this build wrote cannot be
// refused by it: the executor stamps each step while the run is between
// Begin's reading and Finish's, so an honest step sits inside the envelope by
// construction.
//
// The record's envelope is the bound rather than the executor result's, for
// the reason the ingest end gives: the record's two stamps are the only ones
// guaranteed to be stated, while a zero executor window is tolerated all the
// way through. The duration is bounded by the envelope rather than held equal
// to the step's own stamps for the reason given there too, that the executor
// measures it on the monotonic clock while the stamps are wall-clock readings
// of the same instants, so a clock adjustment mid-step makes them disagree
// honestly.
func checkStepWindowsFitTheRecord(entry Entry) error {
	if entry.Result == nil {
		return nil
	}
	if entry.FinishedAt == nil || entry.FinishedAt.IsZero() || entry.StartedAt.IsZero() {
		if len(entry.Result.Steps) == 0 {
			return nil
		}
		return fmt.Errorf("%w: %s cannot be judged against a record stating no envelope",
			errStepOutsideTheRecord, namedStep(entry, 0))
	}
	envelope := entry.FinishedAt.Sub(entry.StartedAt)
	for i, step := range entry.Result.Steps {
		named := namedStep(entry, i)
		switch {
		case step.StartedAt.IsZero() || step.EndedAt.IsZero():
			return fmt.Errorf("%w: %s states no start or no end", errStepOutsideTheRecord, named)
		case step.EndedAt.Before(step.StartedAt):
			return fmt.Errorf("%w: %s ended %s, before it started %s", errStepOutsideTheRecord,
				named, step.EndedAt.UTC(), step.StartedAt.UTC())
		case step.StartedAt.Before(entry.StartedAt):
			return fmt.Errorf("%w: %s began %s, before the record's start stamp %s",
				errStepOutsideTheRecord, named, step.StartedAt.UTC(), entry.StartedAt.UTC())
		case step.EndedAt.After(*entry.FinishedAt):
			return fmt.Errorf("%w: %s ended %s, after the record's finish stamp %s",
				errStepOutsideTheRecord, named, step.EndedAt.UTC(), entry.FinishedAt.UTC())
		case step.Duration < 0 || step.Duration > envelope:
			return fmt.Errorf("%w: %s states a duration of %s, outside the record's %s envelope",
				errStepOutsideTheRecord, named, step.Duration, envelope)
		}
	}
	return nil
}

// namedStep says which step is being refused. The ordinal is what the upload
// and durable history key a step by, and it is stated whatever the name is,
// because a record edited into this state is exactly the one whose step names
// may be missing or repeated.
func namedStep(entry Entry, i int) string {
	name := ""
	if entry.Result != nil && i < len(entry.Result.Steps) {
		name = entry.Result.Steps[i].Name
	}
	if name == "" {
		return fmt.Sprintf("step %d", i)
	}
	return fmt.Sprintf("step %d %q", i, name)
}
