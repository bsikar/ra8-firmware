// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Two doors into the same board transition, and the rule that keeps them
// apart. ApplyBoardCommand is the one an HTTP principal reaches and it refuses
// a system actor outright; TickBoard builds the server-clock actor itself and
// goes straight to the shared path. So "system" is not a kind a request can
// claim, it is a kind only the plane's own clock can be, and the asymmetry is
// the whole security property.
//
// It is observable with no database, because the refusal sits ahead of the
// connection: a Store with no pool separates the two answers, ErrDenied at the
// gate and ErrInvalid past it.

func boardCallerOn(boardID, kind, role string) BoardActor {
	return BoardActor{id: "caller-1", kind: kind, role: role, boardID: boardID}
}

func TestTheHTTPDoorWillNotLetAnyoneBeTheServerClock(t *testing.T) {
	plane := &Store{}
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	tick := board.Tick{Actor: "ra8ci-server"}

	_, _, err := plane.ApplyBoardCommand(context.Background(),
		boardCallerOn("ek-ra8d2", "system", "system"), tick, 0, nil, nil, now)
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("err %v, want ErrDenied: a request claimed the server clock's kind", err)
	}

	// The gate is ahead of the argument check, so a system actor is denied
	// whatever else it sends. Otherwise a caller could learn which of its
	// arguments the plane disliked by claiming to be the clock.
	for _, c := range []struct {
		name    string
		actor   BoardActor
		command board.Command
		now     time.Time
	}{
		{"no command", boardCallerOn("ek-ra8d2", "system", "system"), nil, now},
		{"no clock", boardCallerOn("ek-ra8d2", "system", "system"), tick, time.Time{}},
		{"no board", boardCallerOn("", "system", "system"), tick, now},
		{"another role", boardCallerOn("ek-ra8d2", "system", "operator"), tick, now},
	} {
		if _, _, err := plane.ApplyBoardCommand(context.Background(), c.actor, c.command, 0, nil, nil, c.now); !errors.Is(err, ErrDenied) {
			t.Fatalf("%s: err %v, want ErrDenied", c.name, err)
		}
	}

	// A real principal is not stopped by that gate: it reaches the shared
	// path and is answered on its arguments, which here is the plane having
	// no connection. If this came back denied the gate would be refusing
	// everyone.
	if _, _, err := plane.ApplyBoardCommand(context.Background(),
		boardCallerOn("ek-ra8d2", "human", "operator"), tick, 0, nil, nil, now); !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v, want ErrInvalid: an operator did not reach the transition", err)
	}
}

// TickBoard is the other side of it: the server clock is not refused by the
// rule written against callers claiming to be it. It reaches the shared path
// and is answered on the plane's state, not on its kind.
func TestTheServerClockIsNotRefusedByItsOwnRule(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

	_, _, err := (&Store{}).TickBoard(context.Background(), "ek-ra8d2", 0, now)
	if errors.Is(err, ErrDenied) {
		t.Fatal("the server clock was refused as a caller claiming to be the server clock")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v, want ErrInvalid: the tick did not reach the transition", err)
	}

	// The tick's own arguments are still judged there.
	if _, _, err := (&Store{}).TickBoard(context.Background(), "", 0, now); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a tick for no board: err %v, want ErrInvalid", err)
	}
	if _, _, err := (&Store{}).TickBoard(context.Background(), "ek-ra8d2", 0, time.Time{}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a tick with no clock: err %v, want ErrInvalid", err)
	}
}

// A registration names an external runner, and the name and id are the only
// things about it the plane cannot re-derive later: they are what a scaler
// matches a live GitHub runner against. Judging them before the transaction
// opens is what stops a row being locked to decide a caller sent nothing.
func TestARegistrationNamesARunnerBeforeAnyRowIsLocked(t *testing.T) {
	plane := &Store{}
	reservation := "01996f90-3415-7cfe-8ff1-600058131b10"

	for _, c := range []struct {
		name       string
		runnerID   int64
		runnerName string
	}{
		{"no runner id", 0, "ra8ci-runner-7"},
		{"a negative runner id", -1, "ra8ci-runner-7"},
		{"no runner name", 44, ""},
		{"a runner name too long for the column", 44, string(make([]byte, 257))},
	} {
		if _, err := plane.MarkRunnerVMRegistered(context.Background(), "scaler", reservation, 1, c.runnerID, c.runnerName); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s: err %v, want ErrInvalid", c.name, err)
		}
	}

	// A stated runner identity gets past that check and is answered on the
	// transition's own arguments, so the refusals above are the identity
	// and not the reservation.
	if _, err := plane.MarkRunnerVMRegistered(context.Background(), "scaler", reservation, 1, 44, "ra8ci-runner-7"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v, want ErrInvalid from the transition", err)
	}
	// The bound is inclusive at 256.
	if _, err := plane.MarkRunnerVMRegistered(context.Background(), "scaler", reservation, 1, 44, string(make([]byte, 256))); !errors.Is(err, ErrInvalid) {
		t.Fatalf("err %v, want ErrInvalid from the transition", err)
	}
}
