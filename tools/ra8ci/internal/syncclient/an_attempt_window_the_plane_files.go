// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrUnfilableAttemptWindow is the refusal of a local record whose execution
// stamps fall outside the record that carries them.
var ErrUnfilableAttemptWindow = errors.New("local record states an execution window outside its own envelope")

// checkUploadedAttemptWindowFitsTheRecord holds the executor's own two stamps
// to the record's envelope.
//
// A terminal record states its span twice. The envelope, StartedAt and
// FinishedAt, is the spool's: written around the attempt by the same process
// that froze the record. The execution window, Result.StartedAt and
// Result.EndedAt, is the executor's own account of when the work ran. Nothing
// in this client compared them: the envelope door (#1953's neighbour,
// envelope_the_door_reads.go) judges the spool's pair alone, and the step
// window door judges the steps against that same pair, never the attempt.
//
// spool.checkStampsAreInOrder states this rule at the freeze. That is not this
// door: Pending reads the record back off disk through json.Unmarshal, which
// fills whatever the file holds, so a record written by an older client,
// restored from a backup or edited by hand arrives at the sweep having passed
// nothing. Every door in this file exists for that reason.
//
// THE RULE is the server's own clause, offline.go:138-139: when the execution
// states a start at all, that start may not precede the record's start and the
// execution's end may not follow the record's finish. The comparison matters
// because the two pairs are written by different code from the same clock, and
// a record whose executor claims to have run outside the window the spool
// observed describes no attempt this host could have made.
//
// DELIBERATELY NOT the zero-stamp case. An execution stating no start is not
// refused by the far end, it is CLASSIFIED: offlineInput files a record whose
// Result.StartedAt or Result.EndedAt is zero, or whose Error is set, as
// incomplete_evidence (offline.go:127), which is the plane's way of keeping an
// attempt that came apart before the executor could measure it. Refusing it
// here would throw away the one account durable history has of that failure.
//
// *** HONESTY: the server refuses the out-of-window case, so nothing
// ill-formed was reaching the database. What the refusal buys is where the
// sweep stops and what it says when it does. Refused there, the client reads
// back "upload local <id> returned HTTP 400", which is also what a merely
// unwell server says, and that error ends the whole sweep; Pending re-offers
// the record until a synced marker sits beside it, so every record behind it in
// the outbox waits forever. Refused here, the operator is told which stamp,
// before the bytes leave the host that wrote them.
func checkUploadedAttemptWindowFitsTheRecord(entry spool.Entry) error {
	if entry.Result == nil || entry.FinishedAt == nil || entry.Result.StartedAt.IsZero() {
		return nil
	}
	if entry.Result.StartedAt.Before(entry.StartedAt) {
		return fmt.Errorf("%w: execution began %s, before the record's start stamp %s",
			ErrUnfilableAttemptWindow, entry.Result.StartedAt.UTC(), entry.StartedAt.UTC())
	}
	if entry.Result.EndedAt.After(*entry.FinishedAt) {
		return fmt.Errorf("%w: execution ended %s, after the record's finish stamp %s",
			ErrUnfilableAttemptWindow, entry.Result.EndedAt.UTC(), entry.FinishedAt.UTC())
	}
	return nil
}
