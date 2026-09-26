// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func leaseFor(holder, id, board string) store.LiveBoardLease {
	return store.LiveBoardLease{
		ID: id, BoardID: board, HolderID: holder,
		Priority: "normal", State: "active", Generation: 3,
	}
}

// answeringBench returns the same rows for every holder it is asked about.
type answeringBench struct {
	rows []store.LiveBoardLease
	err  error
}

func (b answeringBench) ListLiveBoardLeasesByHolder(context.Context, string) ([]store.LiveBoardLease, error) {
	return b.rows, b.err
}

func leaseHolderVM() store.RunnerVM {
	return store.RunnerVM{ID: "rsv-1", ExternalRunnerName: "ra8-lab-7"}
}

func TestAnAnswerAboutTheHolderAskedAboutIsReported(t *testing.T) {
	lease, err := heldLease("rsv-1", []store.LiveBoardLease{leaseFor("rsv-1", "lease-1", "board-a")})
	if err != nil {
		t.Fatalf("a lease held by the holder asked about must be reported: %v", err)
	}
	if lease.ID != "lease-1" || lease.BoardID != "board-a" {
		t.Fatalf("reported the wrong lease: %+v", lease)
	}
}

func TestTheFirstRowIsTheReportedOneWhenEveryRowNamesTheHolder(t *testing.T) {
	live := []store.LiveBoardLease{
		leaseFor("rsv-1", "lease-1", "board-a"),
		leaseFor("rsv-1", "lease-2", "board-b"),
	}
	lease, err := heldLease("rsv-1", live)
	if err != nil {
		t.Fatalf("two leases under one holder is still an answer about that holder: %v", err)
	}
	if lease.ID != "lease-1" {
		t.Fatalf("the bench orders by granted_at; report the first row, got %s", lease.ID)
	}
}

func TestARowNamingAnotherHolderIsAContradictionNotAHeldLease(t *testing.T) {
	cases := []struct {
		name string
		live []store.LiveBoardLease
	}{
		{"the only row", []store.LiveBoardLease{leaseFor("rsv-9", "lease-1", "board-a")}},
		{"the first of two", []store.LiveBoardLease{
			leaseFor("rsv-9", "lease-1", "board-a"), leaseFor("rsv-1", "lease-2", "board-b")}},
		{"the second of two", []store.LiveBoardLease{
			leaseFor("rsv-1", "lease-1", "board-a"), leaseFor("rsv-9", "lease-2", "board-b")}},
		{"a row holding nobody", []store.LiveBoardLease{leaseFor("", "lease-1", "board-a")}},
		{"the runner name when the id was asked", []store.LiveBoardLease{
			leaseFor("ra8-lab-7", "lease-1", "board-a")}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := heldLease("rsv-1", tc.live)
			if !errors.Is(err, store.ErrConflict) {
				t.Fatalf("want a conflict about the answer, got %v", err)
			}
			if errors.Is(err, ErrUnclaimedLeaseHeld) {
				t.Fatal("a foreign row must not be reported as a lease this reservation holds")
			}
		})
	}
}

func TestAReportedLeaseMustNameItselfAndItsBoard(t *testing.T) {
	cases := []struct {
		name  string
		lease store.LiveBoardLease
	}{
		{"no lease id", leaseFor("rsv-1", "", "board-a")},
		{"no board", leaseFor("rsv-1", "lease-1", "")},
		{"neither", leaseFor("rsv-1", "", "")},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := heldLease("rsv-1", []store.LiveBoardLease{tc.lease})
			if !errors.Is(err, store.ErrConflict) {
				t.Fatalf("an unactionable refusal is a conflict, got %v", err)
			}
		})
	}
}

func TestAnAnswerWithNoHolderOrNoRowsIsRefusedAsInvalid(t *testing.T) {
	if _, err := heldLease("", []store.LiveBoardLease{leaseFor("rsv-1", "lease-1", "board-a")}); !errors.Is(err, store.ErrInvalid) {
		t.Fatal("asking the bench about no holder is an invalid call")
	}
	if _, err := heldLease("rsv-1", nil); !errors.Is(err, store.ErrInvalid) {
		t.Fatal("heldLease is only reached for a non-empty answer")
	}
}

func TestTheGuardReportsAGenuineLeaseThroughTheStep(t *testing.T) {
	guard, err := NewUnclaimedLeaseGuard(answeringBench{
		rows: []store.LiveBoardLease{leaseFor("rsv-1", "lease-1", "board-a")}})
	if err != nil {
		t.Fatalf("guard: %v", err)
	}
	err = guard.ReleaseReservationLease(context.Background(), leaseHolderVM())
	if !errors.Is(err, ErrUnclaimedLeaseHeld) {
		t.Fatalf("want the held-lease finding, got %v", err)
	}
	for _, want := range []string{"rsv-1", "lease-1", "board-a"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so an operator can find it: %v", want, err)
		}
	}
}

func TestTheGuardDoesNotReportAnotherHoldersLeaseAsThisReservationsHardware(t *testing.T) {
	guard, err := NewUnclaimedLeaseGuard(answeringBench{
		rows: []store.LiveBoardLease{leaseFor("rsv-9", "lease-1", "board-a")}})
	if err != nil {
		t.Fatalf("guard: %v", err)
	}
	err = guard.ReleaseReservationLease(context.Background(), leaseHolderVM())
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want a conflict about the bench answer, got %v", err)
	}
	if errors.Is(err, ErrUnclaimedLeaseHeld) {
		t.Fatal("this is the misdiagnosis the check exists to stop")
	}
}

func TestAnEmptyBenchAnswerIsStillTheOrdinarySuccess(t *testing.T) {
	guard, err := NewUnclaimedLeaseGuard(answeringBench{})
	if err != nil {
		t.Fatalf("guard: %v", err)
	}
	if err := guard.ReleaseReservationLease(context.Background(), leaseHolderVM()); err != nil {
		t.Fatalf("a reservation nobody claimed usually holds no lease: %v", err)
	}
}

// The bench step and the commit step are the two places the unclaimed
// sequence reads somebody else's answer about one reservation. They must
// refuse the same shape of contradiction; this fails if either is relaxed.
func TestBothEndsOfTheSequenceRefuseAnAnswerAboutSomebodyElse(t *testing.T) {
	for _, other := range []string{"rsv-9", "", "ra8-lab-7"} {
		_, leaseErr := heldLease("rsv-1", []store.LiveBoardLease{leaseFor(other, "lease-1", "board-a")})
		releaseErr := checkReleasedReservation("rsv-1", store.RunnerVM{ID: other, State: "released"})
		if !errors.Is(leaseErr, store.ErrConflict) {
			t.Fatalf("lease step accepted an answer about %q: %v", other, leaseErr)
		}
		if !errors.Is(releaseErr, store.ErrConflict) {
			t.Fatalf("commit step accepted an answer about %q: %v", other, releaseErr)
		}
	}
}

// The live-state rule belongs to the bench and is written once, in the store
// query. This package must not grow a second copy of it.
func TestTheLeaseStepDoesNotSecondGuessWhichStatesAreLive(t *testing.T) {
	for _, state := range []string{"pending", "active", "expired", "released", ""} {
		lease := leaseFor("rsv-1", "lease-1", "board-a")
		lease.State = state
		if _, err := heldLease("rsv-1", []store.LiveBoardLease{lease}); err != nil {
			t.Fatalf("state %q was judged here; that rule is store.liveLeaseStates: %v", state, err)
		}
	}
}
