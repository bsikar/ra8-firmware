// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// errContradictoryEnding names the one thing this rule refuses: a result
// handed to Finish stating a step that both ran out of time and was called
// off.
var errContradictoryEnding = errors.New("execution result states a step that ended two ways at once")

// checkEachStepEndedOneWay holds every step to one ending at the freeze.
//
// A step says how it stopped with two independent booleans, TimedOut and
// Cancelled, and Finish wrote both into the outbox having read neither. The
// executor sets them apart: TimedOut is the step's own deadline expiring,
// Cancelled is the run's context going away, and the child is reaped once by
// whichever happened first. A step claiming both describes no run this build
// could have performed.
//
// THE RULE, and it is the store's rather than a new one
// (store.validateLocalRun, local_sync.go:231): not both of a step's two
// endings at once. Both flags are written into local_steps and read back as
// the reason the step stopped, so a row stating both answers the question it
// exists to answer with a contradiction.
//
// This is the FREEZE, not the sweep, and the two doors are not the same door.
// checkUploadedStepEndingsAreOnesThePlaneFiles judges a record read back OFF
// DISK, where an older build's file or an edited one can say anything at all;
// this one judges what the executor in this process just handed over, before
// the bytes are written, so the outbox never holds the claim in the first
// place. Refused at the sweep instead, the record is read, posted, refused
// with an opaque 400 and left in the outbox, and every unsynced record behind
// it waits on every pass.
//
// DELIBERATELY NOT the attempt-level pair, the same cut the sweep's door
// makes. server.offlineInput does not refuse a result that is both, it
// CHOOSES: its switch reads TimedOut first and files the attempt as timed_out
// (offline.go:129-131). Refusing it here would throw away a completed run's
// only account of itself over a pair the plane knows how to read.
func checkEachStepEndedOneWay(result executor.Result) error {
	for i, step := range result.Steps {
		if step.TimedOut && step.Cancelled {
			return fmt.Errorf("%w: %s states it both ran out of time and was called off",
				errContradictoryEnding, namedStepOf(result, i))
		}
	}
	return nil
}
