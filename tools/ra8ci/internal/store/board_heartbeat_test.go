package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

func heartbeatTestBoard(beat time.Time) board.Snapshot {
	granted := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	return board.Snapshot{
		BoardID:        "ek-ra8d2",
		Phase:          board.Active,
		Generation:     7,
		AgentHighWater: 7,
		Version:        41,
		NextSequence:   3,
		Lease: &board.Lease{
			ID: "01996f90-3415-7cfe-8ff1-600058131afe", WaiterID: "01996f90-3415-7cfe-8ff1-600058131afd",
			Holder: "runner-3", Class: board.ClassCI, Reason: "hil run", Generation: 7,
			GrantedAt: granted, ExpiresAt: granted.Add(time.Hour), RequestedDuration: time.Hour,
			DeadlineVersion: 1, LastHeartbeatAt: beat,
		},
	}
}

func beat(s board.Snapshot, at time.Time) board.Snapshot {
	lease := *s.Lease
	lease.LastHeartbeatAt = at
	s.Lease, s.Version = &lease, s.Version+1
	return s
}

func TestBoardWriteForRoutesEachReducerResult(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC)
	before := heartbeatTestBoard(time.Time{})
	heartbeat := board.HolderHeartbeat{Actor: "runner-3", LeaseID: before.Lease.ID, Generation: 7}
	extend := board.Extend{Actor: "runner-3", LeaseID: before.Lease.ID, Generation: 7}

	advanced := before
	advanced.Version++
	events := []board.Event{{Kind: board.LeaseExtended, At: now, BoardID: before.BoardID}}

	if write, err := boardWriteFor(before, advanced, extend, events); err != nil || write != writeTransition {
		t.Fatalf("an event with a version advance did not earn the transition write: %v %v", write, err)
	}
	if _, err := boardWriteFor(before, before, extend, events); !errors.Is(err, ErrConflict) {
		t.Fatal("an event without a version advance was accepted")
	}
	if write, err := boardWriteFor(before, before, extend, nil); err != nil || write != writeNothing {
		t.Fatalf("an unchanged board earned a write: %v %v", write, err)
	}
	// The refusal this path exists to keep: only a command that declares
	// itself event-free may move the version with nothing recorded.
	if _, err := boardWriteFor(before, advanced, extend, nil); !errors.Is(err, ErrConflict) {
		t.Fatal("a silent version advance was accepted for a command that owes an event")
	}
	if write, err := boardWriteFor(before, beat(before, now), heartbeat, nil); err != nil || write != writeLiveness {
		t.Fatalf("a recorded beat did not earn the liveness write: %v %v", write, err)
	}
	skipped := beat(before, now)
	skipped.Version++
	if _, err := boardWriteFor(before, skipped, heartbeat, nil); !errors.Is(err, ErrConflict) {
		t.Fatal("a beat skipping a version was accepted")
	}
}

func TestLivenessOnlyRefusesAnythingButTheBeat(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC)
	before := heartbeatTestBoard(now.Add(-time.Minute))

	if err := livenessOnly(before, beat(before, now)); err != nil {
		t.Fatalf("a later beat was refused: %v", err)
	}
	if err := livenessOnly(before, beat(before, before.Lease.LastHeartbeatAt)); !errors.Is(err, ErrConflict) {
		t.Fatal("a beat that did not move was accepted")
	}
	if err := livenessOnly(before, beat(before, now.Add(-time.Hour))); !errors.Is(err, ErrConflict) {
		t.Fatal("a beat moving backwards was accepted")
	}

	free := before
	free.Lease = nil
	if err := livenessOnly(before, free); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write releasing the lease was accepted")
	}
	if err := livenessOnly(free, beat(before, now)); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write granting a lease was accepted")
	}

	// A deadline is the field this path must never be able to move: an
	// extension is audited and held to a policy limit, and a beat that
	// could carry one would route around both.
	extended := beat(before, now)
	lease := *extended.Lease
	lease.ExpiresAt = lease.ExpiresAt.Add(time.Hour)
	extended.Lease = &lease
	if err := livenessOnly(before, extended); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write lengthening the lease was accepted")
	}

	drained := beat(before, now)
	drained.Phase = board.Draining
	if err := livenessOnly(before, drained); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write changing the phase was accepted")
	}

	queued := beat(before, now)
	queued.Queue = []board.Waiter{{ID: "01996f90-3415-7cfe-8ff1-600058131b01", Holder: "dev", Class: board.ClassHuman}}
	if err := livenessOnly(before, queued); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write changing the queue was accepted")
	}

	regenerated := beat(before, now)
	regenerated.Generation++
	if err := livenessOnly(before, regenerated); !errors.Is(err, ErrConflict) {
		t.Fatal("an event-free write changing the generation was accepted")
	}
}

func TestHeartbeatBindsTheHolderAndRefusesAnyoneElse(t *testing.T) {
	before := heartbeatTestBoard(time.Time{})
	holder := BoardActor{id: "runner-3", kind: "runner", role: "runner", boardID: before.BoardID}
	command := board.HolderHeartbeat{Actor: "someone-else", LeaseID: before.Lease.ID, Generation: 7}

	bound, err := authorizeAndBindCommand(holder, before, command, "")
	if err != nil {
		t.Fatalf("the holder could not report itself alive: %v", err)
	}
	// The claimed actor is replaced by the authenticated one, so a beat
	// cannot be filed on another holder's behalf.
	if bound.(board.HolderHeartbeat).Actor != "runner-3" {
		t.Fatalf("the beat kept the actor the body claimed: %+v", bound)
	}

	other := BoardActor{id: "runner-9", kind: "runner", role: "runner", boardID: before.BoardID}
	if _, err := authorizeAndBindCommand(other, before, command, ""); !errors.Is(err, ErrDenied) {
		t.Fatal("a non-holder was allowed to report the board alive")
	}
	operator := BoardActor{id: "operator-1", kind: "human", role: "operator", boardID: before.BoardID}
	if _, err := authorizeAndBindCommand(operator, before, command, ""); !errors.Is(err, ErrDenied) {
		t.Fatal("an operator was allowed to beat for the holder")
	}
	elsewhere := BoardActor{id: "runner-3", kind: "runner", role: "runner", boardID: "ek-ra8d1"}
	if _, err := authorizeAndBindCommand(elsewhere, before, command, ""); !errors.Is(err, ErrDenied) {
		t.Fatal("a grant on another board was allowed to beat here")
	}

	free := before
	free.Lease = nil
	if _, err := authorizeAndBindCommand(holder, free, command, ""); !errors.Is(err, ErrDenied) {
		t.Fatal("a beat was accepted for a board nobody holds")
	}
}
