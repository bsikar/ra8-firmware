// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A take mints its own durable identifiers before it sends anything, so a
// submission whose answer never arrived can be reconciled rather than
// guessed at. These pin what the caller is handed when the submission does
// not go cleanly, and that a take which already landed is never sent twice.

// queuedTicket enqueues a second waiter on an already-held board and hands
// back the state and the ticket naming that waiter.
func queuedTicket(t *testing.T) (board.Snapshot, Ticket) {
	t.Helper()
	requestID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	leaseID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	state := transition(t, activeBoard(t), board.Enqueue{Actor: "board-agent", Waiter: board.Waiter{
		ID: requestID, LeaseID: leaseID, Holder: "board-agent", Class: board.ClassCI,
		Reason: "test", Duration: time.Minute,
	}})
	return state, Ticket{BoardID: state.BoardID, RequestID: requestID, LeaseID: leaseID}
}

// The take landed and the answer was lost: the retry reads the board, sees
// its own waiter already queued, and stops. Submitting again would put a
// second durable waiter on the board for one caller's one request.
func TestATakeThatAlreadyLandedIsNotSubmittedTwice(t *testing.T) {
	state := activeBoard(t)
	var commands atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		if commands.Add(1) > 1 {
			t.Error("a take that was already queued was submitted again")
		}
		var sent struct {
			RequestID string `json:"request_id"`
			LeaseID   string `json:"lease_id"`
		}
		if err := json.NewDecoder(r.Body).Decode(&sent); err != nil {
			t.Error(err)
		}
		// The waiter landed; only the answer to the caller did not.
		state = transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
			ID: sent.RequestID, LeaseID: sent.LeaseID, Holder: "human", Class: board.ClassHuman,
			Reason: "why not", Duration: time.Minute,
		}})
		jsonResponse(w, http.StatusConflict, map[string]any{
			"code": "conflict", "detail": "expected version is stale", "retryable": true,
		})
	})
	t.Cleanup(done)

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassHuman, "why not", time.Minute)
	if err != nil {
		t.Fatalf("a take that had already landed was reported as failed: %v", err)
	}
	if ticket.RequestID == "" || ticket.LeaseID == "" {
		t.Fatalf("take answered without its identifiers: %+v", ticket)
	}
}

// A board that cannot be read is not a board that refused the take, and the
// caller is handed the identifiers either way so it can reconcile later.
func TestAnUnreadableBoardStillHandsBackTheTicket(t *testing.T) {
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusInternalServerError, map[string]any{"code": "internal", "detail": "boom"})
	})
	t.Cleanup(done)

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassHuman, "why not", time.Minute)
	if err == nil {
		t.Fatal("an unreadable board was reported as an accepted take")
	}
	if ticket.RequestID == "" || ticket.LeaseID == "" {
		t.Fatalf("a failed take was answered without the identifiers to reconcile it: %+v", ticket)
	}
}

// A refused take is handed back once, with its ticket, and not repeated.
func TestARefusedTakeIsHandedBackWithItsTicket(t *testing.T) {
	state := activeBoard(t)
	client, commands := refusing(t, state)

	ticket, err := client.RequestTake(context.Background(), "ek-ra8d2", board.ClassHuman, "why not", time.Minute)
	if err == nil {
		t.Fatal("a refused take was reported as accepted")
	}
	if ticket.RequestID == "" || ticket.LeaseID == "" {
		t.Fatalf("a refused take was answered without its identifiers: %+v", ticket)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d takes, want the refusal handed back after one", got)
	}
}

// A caller that gives up mid-conflict still leaves with the identifiers of
// the submission it may have made.
func TestSubmittingATakeStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	ticket, err := client.RequestTake(ctx, "ek-ra8d2", board.ClassHuman, "why not", time.Minute)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned take to stop", err)
	}
	if ticket.RequestID == "" || ticket.LeaseID == "" {
		t.Fatalf("an abandoned take was answered without its identifiers: %+v", ticket)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d takes after the caller gave up", got)
	}
}

// A cancellation the server refuses is reported, not retried: the waiter is
// still queued and the caller has to know that.
func TestARefusedCancellationIsReported(t *testing.T) {
	state, ticket := queuedTicket(t)
	client, commands := refusing(t, state)

	if err := client.Cancel(context.Background(), ticket); err == nil {
		t.Fatal("a refused cancellation was reported as a removed waiter")
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d cancellations, want the refusal handed back after one", got)
	}
}

// And a caller that gives up mid-conflict stops cancelling.
func TestCancellingStopsWhenTheCallerGivesUp(t *testing.T) {
	state, ticket := queuedTicket(t)
	client, ctx, commands := conflictingForever(t, state)

	if err := client.Cancel(ctx, ticket); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned cancellation to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d cancellations after the caller gave up", got)
	}
}
