// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package board

import (
	"testing"
	"time"
)

// The two refusals that come before this package has any state to reason
// about: New with nothing to name a board by, and Apply handed an argument it
// cannot build a transition from. Both are the caller's mistake rather than
// the board's, and both have to answer in the shape every other refusal here
// uses, an *Error carrying InvalidArgument, so a server can tell a bad
// request from a board that said no.

func TestANewBoardNeedsSomethingToBeNamedBy(t *testing.T) {
	snapshot, err := New("")
	if err == nil {
		t.Fatalf("New(\"\") = %+v, want a refusal", snapshot)
	}
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("New(\"\") refused with %v, want %s", err, InvalidArgument)
	}
	if snapshot.BoardID != "" || snapshot.Phase != "" || snapshot.Lease != nil ||
		len(snapshot.Queue) != 0 || snapshot.Version != 0 || snapshot.NextSequence != 0 {
		t.Fatalf("New(\"\") = %+v, want the zero snapshot beside its refusal", snapshot)
	}
}

func TestANamedBoardStartsReadyAndEmpty(t *testing.T) {
	snapshot, err := New("ra8-lab-board-1")
	if err != nil {
		t.Fatalf("New named board: %v", err)
	}
	if snapshot.BoardID != "ra8-lab-board-1" || snapshot.Phase != Ready ||
		snapshot.Lease != nil || len(snapshot.Queue) != 0 ||
		snapshot.Version != 0 || snapshot.NextSequence != 0 {
		t.Fatalf("New = %+v, want a ready board holding nothing", snapshot)
	}
}

// Apply's front door, before Validate is ever consulted. Each case hands back
// the snapshot it was given rather than a zero one: the caller commits what
// Apply returns, and a refusal that answered with an empty board would have
// the caller write that emptiness over a real row.
func TestApplyRefusesAnIncompleteRequestWithoutTouchingTheBoard(t *testing.T) {
	ready, err := New("ra8-lab-board-1")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	at := time.Date(2026, 9, 26, 9, 0, 0, 0, time.UTC)
	nameless := ready
	nameless.BoardID = ""
	tests := []struct {
		name    string
		before  Snapshot
		command Command
		now     time.Time
	}{
		{name: "no board", before: nameless, command: Enqueue{}, now: at},
		{name: "no command", before: ready, command: nil, now: at},
		{name: "no time", before: ready, command: Enqueue{}, now: time.Time{}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			after, events, err := Apply(test.before, test.command, test.now)
			if !IsCode(err, InvalidArgument) {
				t.Fatalf("Apply refused with %v, want %s", err, InvalidArgument)
			}
			if len(events) != 0 {
				t.Fatalf("events = %+v, want none recorded for a refused request", events)
			}
			if after.BoardID != test.before.BoardID || after.Phase != test.before.Phase ||
				after.Version != test.before.Version {
				t.Fatalf("after = %+v, want the board handed in, unchanged", after)
			}
		})
	}
}
