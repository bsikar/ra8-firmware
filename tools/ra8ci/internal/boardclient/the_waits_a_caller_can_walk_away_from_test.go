// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Waiting for a grant, acknowledging one, and reporting a durable
// generation all sit in a loop that polls the board. Each loop belongs to a
// caller that can walk away, and a loop that keeps polling after its caller
// is gone holds a connection open on behalf of nobody.

// watchedUntilCancelled serves the snapshot to every read and cancels the
// caller's context on the first one, which is the caller giving up while
// its ticket is still only queued.
func watchedUntilCancelled(t *testing.T, state board.Snapshot) (*Client, context.Context, *atomic.Int64) {
	t.Helper()
	var reads atomic.Int64
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		reads.Add(1)
		cancel()
		jsonResponse(w, http.StatusOK, state)
	})
	t.Cleanup(done)
	return client, ctx, &reads
}

// A ticket this client cannot name is refused before a request is spent.
// Waiting on one would be waiting for a grant that was never asked for.
func TestWaitingRefusesATicketItCannotName(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := answering(t, state, nil)
	defer done()

	for name, ticket := range map[string]Ticket{
		"a board ID that is not one":   {BoardID: "not a board id", RequestID: testRequestID, LeaseID: testLeaseID},
		"a request ID that is not one": {BoardID: "ek-ra8d2", RequestID: "request-1", LeaseID: testLeaseID},
		"a lease ID that is not one":   {BoardID: "ek-ra8d2", RequestID: testRequestID, LeaseID: "lease-1"},
		"nothing at all":               {},
	} {
		if _, err := client.WaitForGrant(context.Background(), ticket); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v, want the wait refused", name, err)
		}
	}
	if *commands != 0 {
		t.Fatalf("sent %d commands for tickets that were refused outright", *commands)
	}
}

// A caller that walks away while its waiter is still queued stops the
// wait. The waiter stays on the board, which is what Cancel is for; what
// must not continue is this client polling for it.
func TestWaitingStopsWhenTheCallerWalksAway(t *testing.T) {
	state, ticket := queuedTicket(t)
	client, ctx, reads := watchedUntilCancelled(t, state)

	token, err := client.WaitForGrant(ctx, ticket)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned wait to stop", err)
	}
	if token != (LeaseToken{}) {
		t.Fatalf("an abandoned wait handed back a lease token: %+v", token)
	}
	if got := reads.Load(); got != 1 {
		t.Fatalf("read the board %d times after the caller walked away", got)
	}
}

// Acknowledging a grant races every other transition on the board, so it
// retries a version conflict. It stops retrying when its caller is gone:
// an agent that has given up must not keep claiming it installed a
// generation.
func TestAcknowledgingStopsWhenTheCallerGivesUp(t *testing.T) {
	state := grantPendingBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	if _, err := client.AcknowledgeGrant(ctx, testToken(state)); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned acknowledgement to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d acknowledgements after the caller gave up", got)
	}
}

// And the same for the durable high-water report, which is the observation
// that decides whether the reducer quarantines a restored database.
func TestReportingAGenerationStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	if _, err := client.ObserveAgentGeneration(ctx, state.BoardID, state.Generation); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned report to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d reports after the caller gave up", got)
	}
}
