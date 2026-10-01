// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// Holding a run log page to the window the reader asked for.
//
// getRunLogs already refuses a page that names another attempt, overruns the
// limit, winds the cursor backwards, or moves it further than one page could
// have carried it. What it judged about the chunks themselves was one row:
// the last one has to sit on NextAfter. Everything between the cursor the
// reader sent and that last row went unread.
//
// Two shapes get through that. A chunk at or below `after` is a chunk the
// reader has already been handed, because `after` is the sequence it last
// received; delivered again it is appended to the log a second time, and a
// page whose own last row is at or below `after` leaves the cursor where it
// started, so the next request returns the same page and the reader never
// advances. Chunks out of ascending order are worse and quieter: a log page
// is reassembled in the order it arrives, so stdout written in one order and
// delivered in another reads as a different run. Nothing downstream re-sorts
// them; the sequence is the ordering, and this door is where it is stated.
//
// The rule is one sentence: every chunk on the page falls strictly inside
// (after, NextAfter], strictly ascending, ending on NextAfter. An empty page
// keeps the pair it already had: the cursor may not move, and a page handing
// over nothing may not claim there is more.
//
// This is a stored-state guard, not request validation. AttemptLogs selects
// `c.seq > after ORDER BY c.seq` and holds each scanned row to
// Sequence == NextAfter+1 before appending it, so a page arriving here out of
// order or inside the window means the read path disagrees with itself. That
// is why the caller answers 503 rather than 400, and why this refuses instead
// of sorting the chunks: a page whose own bookkeeping does not add up is not
// a page to hand on with the rows rearranged.
func logPageFitsItsWindow(page store.LogPage, after int64) bool {
	if len(page.Chunks) == 0 {
		return page.NextAfter == after && !page.HasMore
	}
	previous := after
	for _, chunk := range page.Chunks {
		if chunk.Sequence <= previous {
			return false
		}
		previous = chunk.Sequence
	}
	return previous == page.NextAfter
}
