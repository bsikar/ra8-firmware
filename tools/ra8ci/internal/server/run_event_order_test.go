// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func eventsAt(sequences ...int64) []store.RunEvent {
	events := make([]store.RunEvent, 0, len(sequences))
	for _, sequence := range sequences {
		events = append(events, eventAt(sequence))
	}
	return events
}

func TestRunEventOrderJudgesEachShape(t *testing.T) {
	cases := []struct {
		name    string
		events  []store.RunEvent
		after   int64
		accepts bool
	}{
		{"the first event after the cursor", eventsAt(1), 0, true},
		{"a contiguous page", eventsAt(4, 5, 6), 3, true},
		{"gaps are allowed, order is not", eventsAt(4, 9, 40), 3, true},
		{"an empty page is left to the cursor rule", nil, 3, true},
		{"an event sitting on the cursor", eventsAt(3), 3, false},
		{"an event below the cursor", eventsAt(2), 3, false},
		{"the first event already delivered", eventsAt(3, 4, 5), 3, false},
		{"a descending pair", eventsAt(5, 4), 3, false},
		{"one row out of order in the middle", eventsAt(4, 6, 5, 7), 3, false},
		{"the same event twice", eventsAt(4, 4), 3, false},
		{"a page delivered in reverse", eventsAt(6, 5, 4), 3, false},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			page := store.RunEventPage{RunID: "run", Events: testCase.events}
			if got := runEventPageIsAscendingInsideTheWindow(page, testCase.after); got != testCase.accepts {
				t.Fatalf("accepts=%v want %v", got, testCase.accepts)
			}
		})
	}
}

// The store's read path is transcribed rather than called: it needs a
// database. RunEvents selects event_seq > after in ascending order and refuses
// any row whose Sequence is not NextAfter+1, so every page it can build is
// contiguous from after+1. All of them have to pass, at every page size the
// door allows.
func TestRunEventOrderAcceptsEveryPageTheStoreCanBuild(t *testing.T) {
	for _, after := range []int64{0, 1, 49, 100000} {
		for size := 1; size <= store.MaxEventPageSize; size++ {
			page := store.RunEventPage{RunID: "run", Events: eventsFrom(after, size), NextAfter: after + int64(size)}
			if !runEventPageIsAscendingInsideTheWindow(page, after) {
				t.Fatalf("refused a contiguous page: after=%d size=%d", after, size)
			}
		}
	}
}

// Swapping one adjacent pair is the whole difference between the accepted page
// and the refused one, and the cursor is identical across both, so the new
// rule is reading the order rather than repeating the cursor rule.
func TestRunEventOrderSeparatesOnOrderAlone(t *testing.T) {
	ascending := store.RunEventPage{RunID: "run", Events: eventsAt(4, 5, 6, 7), NextAfter: 7}
	swapped := store.RunEventPage{RunID: "run", Events: eventsAt(4, 6, 5, 7), NextAfter: 7}
	if !runEventPageIsAscendingInsideTheWindow(ascending, 3) {
		t.Fatal("ascending page refused")
	}
	if runEventPageIsAscendingInsideTheWindow(swapped, 3) {
		t.Fatal("swapped pair accepted")
	}
	if !runEventCursorFitsPage(ascending, 3) || !runEventCursorFitsPage(swapped, 3) {
		t.Fatal("the cursor rule was expected to accept both, which is why this rule exists")
	}
}

// The two doors of this package now judge a page the same way. The event page
// and the log page carry the same sequences here, so any shape either rule
// accepts, the other must too.
func TestTheEventDoorAgreesWithTheLogDoor(t *testing.T) {
	shapes := [][]int64{
		{1}, {4, 5, 6}, {4, 9, 40}, {3}, {2}, {3, 4, 5}, {5, 4}, {4, 6, 5, 7}, {4, 4}, {6, 5, 4},
	}
	for _, after := range []int64{0, 3} {
		for _, sequences := range shapes {
			events := store.RunEventPage{RunID: "run", Events: eventsAt(sequences...), NextAfter: sequences[len(sequences)-1]}
			chunks := store.LogPage{AttemptID: "attempt", Chunks: chunksAt(sequences...), NextAfter: sequences[len(sequences)-1]}
			eventVerdict := runEventPageIsAscendingInsideTheWindow(events, after) && runEventCursorFitsPage(events, after)
			if got := logPageFitsItsWindow(chunks, after); got != eventVerdict {
				t.Fatalf("after=%d sequences=%v: log door says %v, event door says %v", after, sequences, got, eventVerdict)
			}
		}
	}
}
