// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The last refusals on the two write paths that are decided before any SQL
// runs: which write a reducer result earns, and what a yield sample may be
// filed as.
//
// Both are defence against a defect one layer up rather than against a bad
// request. The reducer already limits a heartbeat to the one field and the
// board package already shapes a sample; these checks derive the same
// properties a second time from the values themselves, so a change in either
// producer is refused at the boundary instead of committed. That makes them
// exactly the checks most likely to go untested, because nothing a caller can
// send will trip them.

// TestABeatCarriesItsRefusalOutOfTheRouter pins the propagation rather than
// the judgement. livenessOnly's refusals are pinned directly elsewhere; what
// matters here is that boardWriteFor hands one back rather than falling
// through to the liveness write, because the liveness write is the one that
// commits a board with no event and no audit row behind it.
func TestABeatCarriesItsRefusalOutOfTheRouter(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC)
	before := heartbeatTestBoard(now.Add(-time.Minute))
	heartbeat := board.HolderHeartbeat{Actor: "runner-3", LeaseID: before.Lease.ID, Generation: 7}

	// A beat that also drains the board: event-free, one version forward,
	// and still not a beat.
	drained := beat(before, now)
	drained.Phase = board.Draining

	write, err := boardWriteFor(before, drained, heartbeat, nil)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("err %v, want ErrConflict", err)
	}
	if write != writeNothing {
		t.Fatalf("write %v, want writeNothing: a refused beat must not earn a write", write)
	}
}

// TestAnEventFreeWriteCannotEditTheQueueInPlace is the queue check's other
// half. Changing the queue's LENGTH is already refused by the count; this is
// the edit that keeps the length and swaps the content, which is what a
// reducer defect would actually look like. Without the element comparison a
// beat could reorder the queue, or replace the waiter at the front of it, with
// no event and no audit row naming who moved.
func TestAnEventFreeWriteCannotEditTheQueueInPlace(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC)
	queued := board.Waiter{
		ID: "01996f90-3415-7cfe-8ff1-600058131b01", LeaseID: "01996f90-3415-7cfe-8ff1-600058131b02",
		Holder: "dev", Class: board.ClassHuman, Reason: "debugging", Sequence: 1,
	}
	before := heartbeatTestBoard(now.Add(-time.Minute))
	before.Queue = []board.Waiter{queued}

	// Same length, different waiter.
	replaced := beat(before, now)
	other := queued
	other.Holder = "someone-else"
	replaced.Queue = []board.Waiter{other}
	if err := livenessOnly(before, replaced); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write replaced the queued waiter")
	}

	// Same waiter, one field moved: a promotion is still a queue edit.
	promoted := beat(before, now)
	moved := queued
	moved.Sequence = 0
	promoted.Queue = []board.Waiter{moved}
	if err := livenessOnly(before, promoted); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write reordered the queue")
	}

	// And the beat with the queue left alone is still taken, or the check
	// would be refusing the one thing this path exists for.
	if err := livenessOnly(before, beat(before, now)); err != nil {
		t.Fatalf("a beat over an unchanged queue was refused: %v", err)
	}
}

// TestAYieldSampleNamesTheBoardItWasMeasuredOn pins the identity refusal. The
// row is filed against a board and a lease, and the estimator reads history
// back by cohort and board; a row missing either is not a weaker measurement,
// it is one that cannot be found again or, worse, one Postgres refuses
// mid-transaction as a constraint violation an operator reads as an opaque
// unavailable.
func TestAYieldSampleNamesTheBoardItWasMeasuredOn(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := asked(t, now, 45*time.Second)
	released := []board.Event{{Kind: board.LeaseReleased, LeaseID: "lease-held", At: now.Add(70 * time.Second), Actor: "ci"}}

	// The same transition with the board named produces a row.
	if _, ok, err := yieldSampleRowFor(before, released); !ok || err != nil {
		t.Fatalf("ok=%v err=%v, want the measured row", ok, err)
	}

	// Without it, the row is refused rather than filed somewhere nobody
	// will look.
	nameless := before
	nameless.BoardID = ""
	row, ok, err := yieldSampleRowFor(nameless, released)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("err %v, want ErrConflict", err)
	}
	if ok || row != (yieldSampleRow{}) {
		t.Fatalf("a refused sample still handed back a row: ok=%v row=%+v", ok, row)
	}
}

// The remaining refusal in yieldSampleRowFor, that a row is neither a
// measurement nor a censored row naming why it is not one, is unreachable
// today: board.YieldSampleFor's terminal switch covers exactly the five kinds
// terminalYieldEvent returns, and every arm sets either the neutral stamp or
// an exclusion reason. It mirrors the table's own CHECK so that a sixth kind
// added upstream is refused here as a named conflict rather than surfacing as
// a constraint violation, and it is left uncovered deliberately rather than
// reached by weakening the board package.
