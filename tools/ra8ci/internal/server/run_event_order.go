// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// Holding the events on a run event page inside the window they were asked
// for, in the order they happened.
//
// runEventCursorFitsPage answers where the cursor lands: NextAfter has to sit
// on the last event delivered. That judges one row. The rows before it were
// never looked at, so a page could deliver them in any order, or deliver ones
// the reader already has, and still end on a cursor that adds up.
//
// Both shapes hurt in the same place. `after` is the sequence the reader last
// received, so an event at or below it is one it has already seen; handed over
// again it is appended to the operator's account of the run a second time, and
// the run reads as though a state change happened twice. Events out of
// ascending order are quieter still: run_events is an append-only log and the
// sequence IS the order, so a page reassembled as it arrives puts the run's
// history in an order the run never had. Nothing downstream re-sorts it.
//
// The log door in this package states exactly this much about its own page
// (logPageFitsItsWindow), and it is the same claim about the same kind of
// cursor-paginated read. Stating it here leaves the two doors of this package
// judging a page by the same rule instead of one being stricter than the
// other.
//
// This is a stored-state guard, not request validation. RunEvents selects
// `event_seq > after ORDER BY event_seq` and holds each scanned row to
// Sequence == NextAfter+1 before appending it, so a page arriving here out of
// order or inside the window means the read path disagrees with itself. The
// caller answers 503 for that, and the rule refuses rather than sorting the
// events: a page whose own bookkeeping does not add up is not a page to hand
// on with the rows rearranged.
//
// An empty page is left to the cursor rule, which already refuses the two
// shapes it can take.
func runEventPageIsAscendingInsideTheWindow(page store.RunEventPage, after int64) bool {
	previous := after
	for _, event := range page.Events {
		if event.Sequence <= previous {
			return false
		}
		previous = event.Sequence
	}
	return true
}
