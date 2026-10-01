// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Two more commands whose retry has to answer to the caller's context, and one
// refusal that must not become a retry at all.
//
// abandonedMidWait covers the commands that conflict on a POST. Waiting for a
// grant never sends one: it reads the board and sleeps, over and over, for as
// long as the queue takes. That sleep is the whole command, so a caller that
// gives up during it is the only way it ever stops early.

// queuedThenAbandoned serves one queued board to every read and gives the
// caller up shortly after the first one, inside the poll rather than during
// the read. The poll is deliberately long here: testClient uses a millisecond,
// which is shorter than the gap being tested, so this builds its own client
// the same way testClient does but with a poll the cancel lands inside.
func queuedThenAbandoned(t *testing.T, state board.Snapshot) (*Client, context.Context, *atomic.Int64) {
	t.Helper()
	var reads atomic.Int64
	var once sync.Once
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		reads.Add(1)
		once.Do(func() {
			go func() {
				time.Sleep(5 * time.Millisecond)
				cancel()
			}()
		})
		jsonResponse(w, http.StatusOK, state)
	}))
	t.Cleanup(server.Close)
	base, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	return &Client{base: base, http: server.Client(), poll: 50 * time.Millisecond}, ctx, &reads
}

// A take that keeps retrying for a caller that has gone would put a waiter on
// the board in the name of nobody, and the ticket it would queue is the one
// thing nobody is left to cancel.
func TestTakingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.RequestTake(ctx, state.BoardID, board.ClassCI, "run the gate", time.Minute)
	stopped(t, "the take", err, commands)
}

// Waiting for a grant is a sleep in a loop, so the caller's context is the
// only thing that ends it before the queue does.
func TestWaitingForAGrantStopsWhenTheCallerGivesUpMidPoll(t *testing.T) {
	state, ticket := queuedTicket(t)
	client, ctx, reads := queuedThenAbandoned(t, state)

	_, err := client.WaitForGrant(ctx, ticket)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("the wait answered %v, want the abandoned caller's own cancellation", err)
	}
	if got := reads.Load(); got < 1 {
		t.Fatalf("the board was never read, so the poll was never under test")
	}
}

// An acknowledgement is the agent saying it durably installed a grant. A board
// that has moved past GrantPending is not a board the ack can be retried
// against: the grant it names is gone, so retrying would beat at a board that
// will never accept it. That is a stale lease, answered once.
func TestAcknowledgingABoardThatMovedOnIsStaleRatherThanRetried(t *testing.T) {
	token := testToken(grantPendingBoard(t))
	moved, err := board.New(token.BoardID)
	if err != nil {
		t.Fatal(err)
	}
	var reads atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			t.Fatalf("an ack was sent to a board that had moved on: %s", r.URL.Path)
		}
		reads.Add(1)
		jsonResponse(w, http.StatusOK, moved)
	})
	t.Cleanup(done)

	if _, err := client.AcknowledgeGrant(context.Background(), token); !errors.Is(err, ErrStaleLease) {
		t.Fatalf("answered %v, want a stale lease", err)
	}
	if got := reads.Load(); got != 1 {
		t.Fatalf("read the board %d times for one stale answer", got)
	}
}

// And the ack's own retry, when the board really is still granting and the
// version moved underneath it.
func TestAcknowledgingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := grantPendingBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.AcknowledgeGrant(ctx, testToken(state))
	stopped(t, "the acknowledgement", err, commands)
}
