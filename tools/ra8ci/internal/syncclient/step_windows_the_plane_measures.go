// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// uploadedClockDisagreement is how far a step's reported duration may exceed
// the span of the stamps it arrives with before the two are a contradiction
// rather than two clocks disagreeing. The executor measures a step's duration
// from a pair of monotonic readings and stamps the same work with their wall
// clock values, so the two differ by a small amount on every real run. This is
// the store's own allowance, localClockDisagreement in store/local_duration.go,
// restated rather than imported: this package does not depend on the store, and
// the number is the one the whole tree already fixes for this comparison.
const uploadedClockDisagreement = 5 * time.Second

// ErrUnmeasuredStepWindow is the refusal of a local record whose steps state a
// window no run could have measured.
var ErrUnmeasuredStepWindow = errors.New("local record states a step window no run could have measured")

// checkUploadedStepWindowsWereMeasured holds every step's two stamps and its
// duration to the record that carries them.
//
// The sweep already asks what each step measured (its exit code, its log
// digests and byte counts) and who each step is (its key, its uniqueness, its
// ending). What it never asked is WHEN, though the step stamps are the fields
// durable history answers "how long did this take" from: server.offlineInput
// copies StartedAt, EndedAt and Duration into LocalStepInput verbatim
// (offline.go:161-162) and re-derives none of them.
//
// spool.checkStepWindowsFitTheRecord states the same rule at the freeze, and
// that is not the same door as this one. Pending reads the record back off disk
// through json.Unmarshal, which fills whatever the file holds, so a record
// written by an older client, restored from a backup, or edited by hand arrives
// at the sweep having passed nothing. Every other door in this file exists for
// that same reason.
//
// THE RULE, the store's own (validateLocalRun local_sync.go:229-231, and
// checkLocalRunDurations): both stamps present, the end at or after the start,
// the step inside the record's own envelope on both sides, the duration not
// negative, and the duration no more than five seconds longer than the span it
// was measured in.
//
// DELIBERATELY NOT the attempt's own duration. The store holds that to the
// record's stamps too, but nothing uploads it: offlineInput computes
// DurationNS from entry.StartedAt and entry.FinishedAt itself (offline.go:114),
// so a Result.Duration disagreeing with the envelope is a number the plane
// never files and refusing it here would throw away a completed run over a
// field durable history does not read.
//
// *** HONESTY: the server refuses all of these, so nothing ill-formed was
// reaching the database. What the refusal buys is where the sweep stops and
// what it says when it does. Refused there, the client reads back "upload local
// <id> returned HTTP 400", which is also what a merely unwell server says, and
// that error ends the whole sweep; Pending re-offers the record until a synced
// marker sits beside it, so the same record is posted and refused on every
// pass and every record behind it in the outbox waits forever. Refused here,
// the operator is told which step and which stamp, before the bytes leave the
// host that wrote them.
//
// The window is judged against the record alone, never against the clock now:
// this host's clock is the one under suspicion, and a rule reading time.Now
// could refuse a record the server would have taken, which is the one direction
// a client-side door must not fail in.
func checkUploadedStepWindowsWereMeasured(entry spool.Entry) error {
	if entry.Result == nil || entry.FinishedAt == nil {
		return nil
	}
	for i, step := range entry.Result.Steps {
		named := uploadedStep(entry, i)
		switch {
		case step.StartedAt.IsZero() || step.EndedAt.IsZero():
			return fmt.Errorf("%w: %s states no start or no end", ErrUnmeasuredStepWindow, named)
		case step.EndedAt.Before(step.StartedAt):
			return fmt.Errorf("%w: %s ended %s, before it started %s",
				ErrUnmeasuredStepWindow, named, step.EndedAt.UTC(), step.StartedAt.UTC())
		case step.StartedAt.Before(entry.StartedAt):
			return fmt.Errorf("%w: %s began %s, before the record's start stamp %s",
				ErrUnmeasuredStepWindow, named, step.StartedAt.UTC(), entry.StartedAt.UTC())
		case step.EndedAt.After(*entry.FinishedAt):
			return fmt.Errorf("%w: %s ended %s, after the record's finish stamp %s",
				ErrUnmeasuredStepWindow, named, step.EndedAt.UTC(), entry.FinishedAt.UTC())
		case step.Duration < 0:
			return fmt.Errorf("%w: %s states a duration of %s", ErrUnmeasuredStepWindow, named, step.Duration)
		}
		if span := step.EndedAt.Sub(step.StartedAt); step.Duration-span > uploadedClockDisagreement {
			return fmt.Errorf("%w: %s reports %s between stamps %s apart",
				ErrUnmeasuredStepWindow, named, step.Duration, span)
		}
	}
	return nil
}

// uploadedStep says which step is being refused. The ordinal is what the upload
// and durable history key a step by, and it is stated whatever the name is,
// because a record edited into this state is exactly the one whose step names
// may be missing.
func uploadedStep(entry spool.Entry, i int) string {
	name := ""
	if entry.Result != nil && i < len(entry.Result.Steps) {
		name = entry.Result.Steps[i].Name
	}
	if name == "" {
		return fmt.Sprintf("step %d", i)
	}
	return fmt.Sprintf("step %d (%q)", i, name)
}
