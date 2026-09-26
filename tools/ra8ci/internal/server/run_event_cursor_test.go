// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"encoding/json"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func eventAt(sequence int64) store.RunEvent {
	return store.RunEvent{
		Sequence:   sequence,
		ID:         "00000000-0000-4000-8000-000000000001",
		Kind:       "state_changed",
		Data:       json.RawMessage(`{"state":"running"}`),
		HappenedAt: time.Unix(1700000000+sequence, 0).UTC(),
	}
}

func eventsFrom(after int64, count int) []store.RunEvent {
	events := make([]store.RunEvent, 0, count)
	for i := 1; i <= count; i++ {
		events = append(events, eventAt(after+int64(i)))
	}
	return events
}

func TestACursorEndingOnTheLastDeliveredEventIsAccepted(t *testing.T) {
	for _, tc := range []struct {
		name  string
		after int64
		count int
	}{
		{"one event from the start", 0, 1},
		{"a short page", 0, 3},
		{"a full page", 0, store.MaxEventPageSize},
		{"resuming mid-log", 41, 4},
		{"resuming on a single event", 900, 1},
	} {
		t.Run(tc.name, func(t *testing.T) {
			events := eventsFrom(tc.after, tc.count)
			page := store.RunEventPage{
				RunID:     "run",
				Events:    events,
				NextAfter: events[len(events)-1].Sequence,
			}
			if !runEventCursorFitsPage(page, tc.after) {
				t.Fatalf("page ending on its own last event was refused")
			}
		})
	}
}

func TestACursorPastTheLastDeliveredEventIsRefused(t *testing.T) {
	for _, tc := range []struct {
		name  string
		next  int64
		after int64
	}{
		{"one event skipped", 4, 0},
		{"the rest of the page skipped", 50, 0},
		{"a cursor that never moved off the request", 0, 0},
		{"a cursor short of the page", 2, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			page := store.RunEventPage{RunID: "run", Events: eventsFrom(tc.after, 3), NextAfter: tc.next}
			if runEventCursorFitsPage(page, tc.after) {
				t.Fatalf("cursor %d was accepted against a page ending at 3", tc.next)
			}
		})
	}
}

// The reason the rule exists: `after` only moves forward, so a range the
// cursor jumps is a range no later request asks for.
func TestTheSkippedRangeIsUnreachableOnTheNextRequest(t *testing.T) {
	page := store.RunEventPage{RunID: "run", Events: eventsFrom(0, 2), NextAfter: 9}
	if runEventCursorFitsPage(page, 0) {
		t.Fatalf("a page delivering events 1-2 while pointing at 9 was accepted")
	}
	// Events 3 through 9 were never delivered and the reader's next
	// request starts after 9, which is precisely what is being refused.
	if page.Events[len(page.Events)-1].Sequence >= page.NextAfter {
		t.Fatalf("test fixture does not model a skipped range")
	}
}

func TestAnEmptyPageMayNotMoveTheCursor(t *testing.T) {
	for _, after := range []int64{0, 1, 77} {
		page := store.RunEventPage{RunID: "run", Events: nil, NextAfter: after}
		if !runEventCursorFitsPage(page, after) {
			t.Fatalf("an empty page holding the cursor at %d was refused", after)
		}
		moved := store.RunEventPage{RunID: "run", Events: nil, NextAfter: after + 1}
		if runEventCursorFitsPage(moved, after) {
			t.Fatalf("an empty page advancing the cursor past %d was accepted", after)
		}
	}
}

func TestAnEmptyPageMayNotClaimMore(t *testing.T) {
	page := store.RunEventPage{RunID: "run", Events: nil, NextAfter: 5, HasMore: true}
	if runEventCursorFitsPage(page, 5) {
		t.Fatalf("an empty page claiming more was accepted")
	}
}

// A populated page is judged on its cursor alone here; HasMore is the existing
// door's question and stays there, so this must not start answering it.
func TestAPopulatedPageIsNotJudgedOnHasMore(t *testing.T) {
	events := eventsFrom(0, 3)
	for _, more := range []bool{false, true} {
		page := store.RunEventPage{RunID: "run", Events: events, NextAfter: 3, HasMore: more}
		if !runEventCursorFitsPage(page, 0) {
			t.Fatalf("a consistent cursor was refused with has_more=%v", more)
		}
	}
}

// The server door and the runner client must apply one rule, not two that
// drift. This crosses them over the same pages.
func TestTheServerDoorAgreesWithTheClientCursorRule(t *testing.T) {
	clientRule := func(page store.RunEventPage, after int64) bool {
		sequence := after
		for _, event := range page.Events {
			sequence = event.Sequence
		}
		return page.NextAfter == sequence && !(len(page.Events) == 0 && page.HasMore)
	}
	pages := []struct {
		page  store.RunEventPage
		after int64
	}{
		{store.RunEventPage{Events: eventsFrom(0, 3), NextAfter: 3}, 0},
		{store.RunEventPage{Events: eventsFrom(0, 3), NextAfter: 4}, 0},
		{store.RunEventPage{Events: eventsFrom(0, 3), NextAfter: 0}, 0},
		{store.RunEventPage{Events: nil, NextAfter: 7}, 7},
		{store.RunEventPage{Events: nil, NextAfter: 8}, 7},
		{store.RunEventPage{Events: nil, NextAfter: 7, HasMore: true}, 7},
		{store.RunEventPage{Events: eventsFrom(12, 1), NextAfter: 13}, 12},
	}
	for i, tc := range pages {
		if got, want := runEventCursorFitsPage(tc.page, tc.after), clientRule(tc.page, tc.after); got != want {
			t.Fatalf("page %d: server door says %v, client rule says %v", i, got, want)
		}
	}
}
