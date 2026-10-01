// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
)

// errRecordCannotBeUploaded names the one thing this rule refuses: a terminal
// record sitting in the outbox that no ingest end can ever accept.
var errRecordCannotBeUploaded = errors.New("terminal record cannot describe the run it reports")

// checkTerminalRecordCarriesItsRun holds a record on disk to the evidence the
// upload sends on its behalf.
//
// Pending already re-applies checkFinishMatchesStart at this door, for the
// reason stated there: Finish can only hold what the caller hands it, and this
// holds what is on disk, which is what SyncPending marshals. The record's own
// two other conditions were left at the Finish door alone. Finish writes a
// result for every terminal record (it takes one by value and stores its
// address) and refuses stamps that are out of order, so a record on disk
// missing either was not written by this build's Finish: an older build wrote
// it before those rules existed, a different tool wrote into the directory, or
// the file was edited afterwards, which is exactly the case
// pending_matches_start already exists to catch for the frozen fields.
//
// What such a record costs is not itself. offlineInput refuses a nil result
// outright and refuses stamps out of order twice over, so the upload answers
// 400, and SyncPending returns on the first response that is not 200. Every
// unsynced record behind it therefore waits on this pass and on every pass
// after it, and Pending hands the same record up again each time, because a
// record is only retired once a synced marker sits beside it. One unusable
// file at the front of the outbox stops a disconnected host from ever
// delivering the evidence it kept, and the operator reads back
// "upload local <id> returned HTTP 400" with no field named.
//
// Refused here, the record costs only itself, the pass names the file and what
// is wrong with it, and the rest of the outbox is still behind a door that
// will open. It is refused rather than skipped for the same reason the
// receipt rule gives: skipping it silently would retire evidence the server
// never received, and this package's answer to a record it cannot judge is to
// stop and say so.
func checkTerminalRecordCarriesItsRun(entry Entry) error {
	if entry.Result == nil {
		return fmt.Errorf("%w: no execution result", errRecordCannotBeUploaded)
	}
	if err := checkStampsAreInOrder(entry); err != nil {
		return fmt.Errorf("%w: %w", errRecordCannotBeUploaded, err)
	}
	// The steps the result carries are uploaded with it and judged by the
	// same ingest end, so they are held to the record here for the reason
	// stated above: refused there, the whole outbox waits behind this file.
	if err := checkStepWindowsFitTheRecord(entry); err != nil {
		return fmt.Errorf("%w: %w", errRecordCannotBeUploaded, err)
	}
	return nil
}
