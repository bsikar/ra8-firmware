// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrContradictoryEnding is the refusal of a record stating a step that both
// ran out of time and was called off.
var ErrContradictoryEnding = errors.New("local record states a step that ended two ways at once")

// checkUploadedStepEndingsAreOnesThePlaneFiles holds every step to one ending.
//
// A step says how it stopped with two independent booleans, TimedOut and
// Cancelled, and the sweep has never read either: its exit door judges the
// numbers (#1953), its evidence door judges the digests and counts (#1955),
// and its step door judges the names and the count (#1963). The pair was the
// last thing a step states that nothing on this host looks at.
//
// The two cannot both be true of one step. The executor sets them apart:
// TimedOut is the step's own deadline expiring, Cancelled is the run's
// context going away, and the child is reaped once by whichever happened.
// They also mean different things downstream, which is why the far end cares:
// store.validateLocalRun refuses `step.TimedOut && step.Cancelled` outright
// (local_sync.go:231), and both flags are written into local_steps and read
// back as the reason the step stopped.
//
// A record stating both describes no run: the step ran out of time AND was
// called off before it did. It reaches the plane as one more opaque 400, with
// the familiar cost: SyncPending returns on the first response that is not
// 200, so an unfilable record at the front of the outbox holds every record
// behind it on this pass and on every pass after, and the operator reads back
// "upload local <id> returned HTTP 400" with no step and no field named.
//
// DELIBERATELY NOT the attempt-level pair. offlineInput does not refuse a
// result that is both, it CHOOSES: its switch reads TimedOut first, so such a
// record is filed as timed_out rather than rejected (offline.go:129-131).
// Refusing it here would throw away evidence the plane accepts, which is the
// same cut every other door in this package makes. Pinned by
// TestTheAttemptLevelPairIsLeftToTheDoorThatChooses.
func checkUploadedStepEndingsAreOnesThePlaneFiles(entry spool.Entry) error {
	if entry.Result == nil {
		return nil
	}
	for i, step := range entry.Result.Steps {
		if step.TimedOut && step.Cancelled {
			return fmt.Errorf("%w: step %d states it both ran out of time and was called off",
				ErrContradictoryEnding, i)
		}
	}
	return nil
}
