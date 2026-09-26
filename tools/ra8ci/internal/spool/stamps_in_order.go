// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
)

// errStampsOutOfOrder names the one thing this rule refuses: a terminal record
// whose own stamps cannot describe a run that happened.
var errStampsOutOfOrder = errors.New("terminal record's stamps are out of order")

// checkStampsAreInOrder holds a terminal record's stamps to the run they claim.
//
// Both stamps this package writes come from the local host's clock and nothing
// in between reads them back: Begin takes time.Now().UTC() before the first
// command, Finish takes it again when the caller hands the result back, and
// the record is written with whatever the two readings were. A clock that
// steps backwards between them (an NTP correction on a host that has just come
// back online, a suspended VM resuming, an operator setting the date) produces
// a record that finished before it started, and this package wrote it without
// comment. The executor's own window is carried inside the same record and can
// fall outside it the same way, because Result.EndedAt is read before Finish
// takes its reading.
//
// The offline path is where this bites. Every door that record has left to
// pass refuses it: offlineInput ends with StartedAt.After(*FinishedAt), it
// refuses an executor window outside the record's envelope just above that,
// and store.validateLocalRun refuses FinishedAt.Before(StartedAt) again before
// the insert. So the record cannot be ingested, ever. It is not only lost:
// syncclient.SyncPending returns on the first upload that does not answer 200,
// so an unusable record at the front of the outbox ends the sweep and every
// other unsynced record behind it waits on this pass and on every pass after,
// with the client reading back "upload local <id> returned HTTP 400" and no
// field named. The whole point of the outbox is that a disconnected host keeps
// its evidence until a server is reachable, and those are exactly the hosts
// whose clocks step when they reconnect.
//
// Refusing at Finish costs the record that would have been refused anyway and
// reports it at the host, in the moment, naming both stamps; the run's output
// has already been streamed to the operator by the executor, and the start
// record stays on disk stating what the spool already means by an attempt that
// began and never reported a result.
func checkStampsAreInOrder(entry Entry) error {
	if entry.FinishedAt == nil {
		return fmt.Errorf("%w: no finish stamp", errStampsOutOfOrder)
	}
	if entry.FinishedAt.Before(entry.StartedAt) {
		return fmt.Errorf("%w: finish stamp %s precedes start stamp %s",
			errStampsOutOfOrder, entry.FinishedAt.UTC(), entry.StartedAt.UTC())
	}
	if entry.Result == nil {
		return nil
	}
	if !entry.Result.StartedAt.IsZero() && entry.Result.StartedAt.Before(entry.StartedAt) {
		return fmt.Errorf("%w: execution began %s, before the record's start stamp %s",
			errStampsOutOfOrder, entry.Result.StartedAt.UTC(), entry.StartedAt.UTC())
	}
	if !entry.Result.EndedAt.IsZero() && entry.Result.EndedAt.After(*entry.FinishedAt) {
		return fmt.Errorf("%w: execution ended %s, after the record's finish stamp %s",
			errStampsOutOfOrder, entry.Result.EndedAt.UTC(), entry.FinishedAt.UTC())
	}
	return nil
}
