// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// Holding a run event page's cursor to the events it actually delivered.
//
// getRunEvents already refuses a page that names another run, overruns the
// limit, winds the cursor backwards, or claims more while handing over a short
// page. What it never asked is the question the cursor exists for: does
// NextAfter point at the last event on this page?
//
// It matters because `after` only ever moves forward. A reader pages with the
// NextAfter it was handed, so a cursor that sits ahead of the last delivered
// event names a range that reader will never ask for again, and the events in
// it are unreachable through this door from then on. Run events are the
// operator's account of what happened to a run, so the loss is silent and
// permanent rather than an error anyone sees. The two shapes are an empty page
// that still advances the cursor, and a populated page whose cursor has run
// past its own last row.
//
// The rule is stated here because the log door in this package already states
// exactly this much about its own cursor (getRunLogs: an empty page may not
// move NextAfter, and a populated one must end on it), and because the runner
// client states it too, on the far side of the wire
// (runclient.Client.Events). The server was the one end judging a cursor by
// its bounds rather than by the page under it, which left the full rule
// applying only to callers that happen to be that Go client. curl, a CI
// script and every other reader got the weaker one.
//
// This is a stored-state guard, not request validation: the store's own query
// builds NextAfter from the rows it appended and holds each event to
// Sequence == NextAfter+1, so a page reaching here inconsistent means the read
// path disagrees with itself. That is why the caller answers 503 rather than
// 400, and why this refuses rather than repairs the cursor: a page whose own
// bookkeeping does not add up is not a page to hand on with the number
// corrected.
func runEventCursorFitsPage(page store.RunEventPage, after int64) bool {
	if len(page.Events) == 0 {
		// Nothing was delivered, so there is nothing for the cursor to
		// have advanced past. HasMore is refused with it: a page that
		// reports more while handing over none leaves a reader asking
		// for the same empty range forever.
		return page.NextAfter == after && !page.HasMore
	}
	return page.Events[len(page.Events)-1].Sequence == page.NextAfter
}
