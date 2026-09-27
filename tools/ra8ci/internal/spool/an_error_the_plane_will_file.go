// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"
	"unicode/utf8"
)

// errUnfilableError names the one thing this rule refuses: an executor error
// frozen into a terminal record that no plane can file the run with.
var errUnfilableError = errors.New("local run error the plane will not file")

// checkTheErrorIsOneThePlaneWillFile holds the message a failing run is filed
// with to the two spellings a text column cannot hold at any length.
//
// This is the third string at this pair of doors to be left to the column
// that holds it, after the arguments and the three identities, and it is the
// one with the strongest claim to being stated on the host. Finish writes
// runErr.Error() into entry.Error and looks at nothing about it. Every other
// string in a local record is chosen from a reviewed definition or read off
// the checkout; this one is whatever a failing executor wrapped into an
// error, and a child's own output is an ordinary thing to wrap. Bytes read
// off a pipe are not UTF-8 because a Go string holds them.
//
// IngestLocalRun writes the string straight into local_runs.executor_error,
// text NOT NULL DEFAULT ” (0005_offline_sync.sql), and a Postgres text
// column holds no NUL and no invalid UTF-8 whatever its length.
// store.executorErrorATextColumnCanHold says so at the far end and states
// what it costs: refused there, the two spellings reach the INSERT inside the
// ingest transaction and fail there, so the operator reads an unavailable
// store rather than the invalid record it is.
//
// Refusing at Finish is what the timing is worth. The record is already
// written by the time this message exists, so nothing here saves the work;
// what it saves is the outbox. syncclient.SyncPending returns on the first
// upload that is not 200, and Pending re-offers a record until a synced
// marker sits beside it, so one unfilable record at the front holds every
// record behind it on that pass and on every pass after. Refused here, the
// failure is named on the host, in the moment, against the run that produced
// it, instead of surfacing weeks later as an unavailable store on the first
// pass that finally reaches one.
//
// DELIBERATELY NOT A LENGTH RULE, though the far end has one:
// validateLocalRun bounds the field at 1024 bytes. Length is different in
// kind from the two spellings above. A message too long is refused cleanly by
// that door as the invalid record it is, with the field named, and it is
// caught earlier still by syncclient's own record-size door, which stops an
// oversized record before the request and leaves it in the outbox to be read.
// A record carrying a long but readable message is therefore evidence someone
// can still act on, and refusing it here would throw away a completed run's
// only account of itself over text a person could have read. The NUL and the
// invalid byte have no such reading anywhere.
//
// DELIBERATELY NOT a claim about WHEN a message may be present. A non-empty
// error makes offlineInput classify the run "incomplete_evidence", and the
// column's CHECK refuses an error beside a success, but that verdict is the
// server's to reach from the record as a whole; this door judges the text and
// nothing else.
func checkTheErrorIsOneThePlaneWillFile(message string) error {
	if !utf8.ValidString(message) {
		return fmt.Errorf("%w: the message is not valid UTF-8", errUnfilableError)
	}
	for _, char := range message {
		if char == 0 {
			return fmt.Errorf("%w: the message holds a NUL", errUnfilableError)
		}
	}
	return nil
}
