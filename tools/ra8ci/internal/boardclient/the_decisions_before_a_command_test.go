// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Cancelling a ticket, finishing a recovery and reading HIL history all
// decide what they may do from the snapshot in front of them. These hold
// the decisions taken before a command is sent, each proved against a
// server that counts commands so a refusal is shown to have reached
// nobody. countedBoard comes from the_grant_this_agent_acknowledges_test.go.

// silentProducer stands in for the neutral attestor and records whether it
// was ever asked for a receipt.
type silentProducer struct{ asked int }

func (p *silentProducer) ProduceNeutralReceipt(context.Context, store.NeutralChallenge) ([]byte, error) {
	p.asked++
	return []byte("receipt"), nil
}

func TestCancelRefusesATicketItCannotBeAbout(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	good := Ticket{BoardID: state.BoardID, RequestID: testRequestID, LeaseID: testLeaseID}
	noBoard := good
	noBoard.BoardID = "not a board id"
	noRequest := good
	noRequest.RequestID = "request-4"
	noLease := good
	noLease.LeaseID = ""

	for name, ticket := range map[string]Ticket{
		"a board ID that is not one":   noBoard,
		"a request ID that is not one": noRequest,
		"no lease ID":                  noLease,
	} {
		if err := client.Cancel(context.Background(), ticket); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused cancellations still reached the server", commands.Load())
	}
}

// A ticket that already holds the board cannot be cancelled: the caller is
// told it was granted rather than having the lease quietly torn down.
func TestCancelRefusesToUnpickAGrantedTicket(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	ticket := Ticket{BoardID: state.BoardID, RequestID: state.Lease.WaiterID, LeaseID: state.Lease.ID}
	if err := client.Cancel(context.Background(), ticket); !errors.Is(err, ErrAlreadyGranted) {
		t.Fatalf("cancelling a granted ticket = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatal("a granted ticket was cancelled at the server")
	}
}

// A ticket the board has never heard of is already in the state the caller
// wanted, so cancelling it succeeds without spending a command.
func TestCancelIsSatisfiedByATicketTheBoardDoesNotHold(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	absent := Ticket{BoardID: state.BoardID, RequestID: testProofID, LeaseID: testProofID}
	if err := client.Cancel(context.Background(), absent); err != nil {
		t.Fatalf("cancelling a ticket the board never held = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatal("a ticket the board never held was cancelled at the server")
	}
}

// Finishing a recovery needs a board that is recovering and an attestor
// that can answer for it. Both are judged before a challenge is asked for,
// and the two failures are told apart because they are fixed differently.
func TestFinishRecoveryRefusesWhatItCannotAttest(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	producer := &silentProducer{}
	for _, boardID := range []string{"", "not a board id", "ek/ra8d2"} {
		if _, err := client.FinishRecovery(context.Background(), boardID, producer); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("board ID %q = %v", boardID, err)
		}
	}
	if _, err := client.FinishRecovery(context.Background(), state.BoardID, nil); !errors.Is(err, ErrNeutralUnavailable) {
		t.Fatalf("no attestor = %v", err)
	}
	if _, err := client.FinishRecovery(context.Background(), state.BoardID, producer); !errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("a board that is not recovering = %v", err)
	}
	if producer.asked != 0 {
		t.Fatalf("the attestor was asked for %d receipts it could not be about", producer.asked)
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused recoveries still reached the server", commands.Load())
	}
}

// HIL history is only ever asked for on behalf of a validated HIL task on
// this board, so a task of another shape is refused before a request is
// spent.
func TestHILObservationsRefusesATaskItCannotBeAbout(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	hil := catalog.Task{Name: "hil-smoke", Scope: "hil", HIL: &catalog.HILTask{BoardID: state.BoardID}}
	otherScope := hil
	otherScope.Scope = "unit"
	noHIL := hil
	noHIL.HIL = nil
	otherBoard := hil
	otherBoard.HIL = &catalog.HILTask{BoardID: "ek-ra8m1"}

	for name, task := range map[string]catalog.Task{
		"a task that is not HIL":            otherScope,
		"a HIL task with no HIL part":       noHIL,
		"a HIL task for another board":      otherBoard,
		"a HIL task that does not validate": hil,
	} {
		if _, _, err := client.HILObservations(context.Background(), state.BoardID, task); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if _, _, err := client.HILObservations(context.Background(), "not a board id", hil); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("a board ID that is not one = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused history requests still reached the server", commands.Load())
	}
}

var _ board.Phase = board.Ready
