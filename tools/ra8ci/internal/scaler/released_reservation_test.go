// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// verbatimCloser answers with exactly the row the test states, including its
// ID. fakeCloser stamps the asked-for ID onto its answer, which is the one
// thing this door exists to stop being assumed.
type verbatimCloser struct {
	answer store.RunnerVM
	asked  []string
}

func (c *verbatimCloser) ReleaseUnclaimedRunnerVM(_ context.Context, _, id string) (store.RunnerVM, error) {
	c.asked = append(c.asked, id)
	return c.answer, nil
}

func closingWith(t *testing.T, answer store.RunnerVM) (*UnclaimedRevocation, *verbatimCloser) {
	t.Helper()
	closer := &verbatimCloser{answer: answer}
	revocation, err := NewUnclaimedRevocation("reaper", &fakeRegistrations{}, &fakeGuests{}, &fakeLeases{}, closer)
	if err != nil {
		t.Fatalf("new unclaimed revocation: %v", err)
	}
	return revocation, closer
}

func TestAReleasedRowNamingAnotherReservationIsRefused(t *testing.T) {
	err := checkReleasedReservation("res-1", store.RunnerVM{ID: "res-2", State: "released"})
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want a conflict for a row about another reservation, got %v", err)
	}
}

func TestAReleasedRowForTheAskedReservationPasses(t *testing.T) {
	if err := checkReleasedReservation("res-1", store.RunnerVM{ID: "res-1", State: "released"}); err != nil {
		t.Fatalf("want the asked reservation accepted, got %v", err)
	}
}

// Identity is judged first on purpose: another reservation's row can perfectly
// well say released, and reading that state would be reading somebody else's.
func TestAnotherReservationIsRefusedEvenWhenItIsReleased(t *testing.T) {
	err := checkReleasedReservation("res-1", store.RunnerVM{ID: "res-2", State: "released"})
	if err == nil || !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want a conflict, got %v", err)
	}
	if got := err.Error(); got == "" {
		t.Fatal("want the contradiction stated")
	}
}

func TestAStateOtherThanReleasedIsRefused(t *testing.T) {
	for _, state := range []string{"", "reserved", "stopped", "draining", "running", "registered"} {
		err := checkReleasedReservation("res-1", store.RunnerVM{ID: "res-1", State: state})
		if !errors.Is(err, store.ErrConflict) {
			t.Fatalf("state %q: want a conflict, got %v", state, err)
		}
	}
}

func TestAnEmptyReservationIsNotSomethingToAskAbout(t *testing.T) {
	err := checkReleasedReservation("", store.RunnerVM{ID: "", State: "released"})
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("want invalid for an unnamed reservation, got %v", err)
	}
}

// An empty row is the shape a store returns beside an error it did not
// report. It names no reservation, so it is refused rather than read.
func TestAZeroRowIsRefused(t *testing.T) {
	err := checkReleasedReservation("res-1", store.RunnerVM{})
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want a conflict for a zero row, got %v", err)
	}
}

func TestAbandonAttemptRefusesALedgerRowAboutAnotherReservation(t *testing.T) {
	asked := unclaimedVM("running")
	revocation, closer := closingWith(t, store.RunnerVM{ID: "b0a1c2d3-0000-4000-8000-000000000002", State: "released"})
	err := revocation.AbandonAttempt(context.Background(), asked)
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("abandon returned %v, want a conflict", err)
	}
	if len(closer.asked) != 1 || closer.asked[0] != asked.ID {
		t.Fatalf("want the release asked about %s, got %v", asked.ID, closer.asked)
	}
}

func TestAbandonAttemptAcceptsTheReservationItAskedAbout(t *testing.T) {
	asked := unclaimedVM("running")
	revocation, _ := closingWith(t, store.RunnerVM{ID: asked.ID, State: "released"})
	if err := revocation.AbandonAttempt(context.Background(), asked); err != nil {
		t.Fatalf("want the asked reservation closed, got %v", err)
	}
}
