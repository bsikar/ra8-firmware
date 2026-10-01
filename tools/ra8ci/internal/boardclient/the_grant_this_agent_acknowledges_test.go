// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Acknowledging a grant and reporting a generation are the two steps that
// tell the server what the agent durably installed, so a request that
// cannot be about this lease is refused before it is sent. These hold the
// refusals and the idempotent answers; the ordinary acknowledge round trip
// is already held by client_test.go.

// countedBoard serves one snapshot and counts every command the client
// tries to send, so a test can prove a refusal never reached the server.
func countedBoard(t *testing.T, state board.Snapshot) (*Client, *atomic.Int64, func()) {
	t.Helper()
	var commands atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			commands.Add(1)
		}
		jsonResponse(w, http.StatusOK, state)
	})
	return client, &commands, done
}

func TestAcknowledgeGrantRefusesATokenItCannotBeAbout(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	token := testToken(state)
	noBoard := token
	noBoard.BoardID = "not a board id"
	noRequest := token
	noRequest.RequestID = "request-4"
	noLease := token
	noLease.LeaseID = ""
	noGeneration := token
	noGeneration.Generation = 0

	var noContext context.Context
	if _, err := client.AcknowledgeGrant(noContext, token); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("no context = %v", err)
	}
	for name, bad := range map[string]LeaseToken{
		"a board ID that is not one":   noBoard,
		"a request ID that is not one": noRequest,
		"no lease ID":                  noLease,
		"no generation":                noGeneration,
	} {
		if _, err := client.AcknowledgeGrant(context.Background(), bad); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused acknowledgements still reached the server", commands.Load())
	}
}

// An agent that already installed this generation is told so without a
// second command being sent, which is what makes a retry after an
// ambiguous response safe.
func TestAcknowledgeGrantIsIdempotentOnceInstalled(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	snapshot, err := client.AcknowledgeGrant(context.Background(), testToken(state))
	if err != nil {
		t.Fatalf("acknowledge an installed generation: %v", err)
	}
	if snapshot.Lease.Generation != state.Lease.Generation || snapshot.AgentHighWater != state.AgentHighWater {
		t.Fatalf("snapshot = %+v", snapshot)
	}
	if commands.Load() != 0 {
		t.Fatal("an already-installed generation was acknowledged a second time")
	}
}

// A token that names another lease, or another generation of this one, is
// stale: the board moved on while this agent was away.
func TestAcknowledgeGrantRefusesAnotherLeaseOrGeneration(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	otherLease := testToken(state)
	otherLease.LeaseID = testProofID
	otherGeneration := testToken(state)
	otherGeneration.Generation = state.Lease.Generation + 1

	for name, token := range map[string]LeaseToken{
		"another lease":      otherLease,
		"another generation": otherGeneration,
	} {
		if _, err := client.AcknowledgeGrant(context.Background(), token); !errors.Is(err, ErrStaleLease) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d stale acknowledgements still reached the server", commands.Load())
	}
}

func TestObserveAgentGenerationRefusesARequestItCannotSend(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	var noContext context.Context
	if _, err := client.ObserveAgentGeneration(noContext, state.BoardID, 1); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("no context = %v", err)
	}
	for _, boardID := range []string{"", "not a board id", "ek/ra8d2"} {
		if _, err := client.ObserveAgentGeneration(context.Background(), boardID, 1); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("board ID %q = %v", boardID, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused observations still reached the server", commands.Load())
	}
}

// Waiting for a yield is a poll, so it refuses a request it could never
// answer before it starts polling. The yield transition itself is already
// held by yield_test.go.
func TestWaitForYieldRequestRefusesARequestItCannotPoll(t *testing.T) {
	granted := activeBoard(t)

	var absent *Client
	if _, err := absent.WaitForYieldRequest(context.Background(), testToken(granted)); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("no client = %v", err)
	}
	client, commands, done := countedBoard(t, granted)
	defer done()
	var noContext context.Context
	if _, err := client.WaitForYieldRequest(noContext, testToken(granted)); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("no context = %v", err)
	}
	unbounded := testToken(granted)
	unbounded.ExpiresAt = time.Time{}
	if _, err := client.WaitForYieldRequest(context.Background(), unbounded); !errors.Is(err, ErrInvalidRequest) {
		t.Fatalf("a token with no expiry = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused waits still commanded the server", commands.Load())
	}
}

// An active board has nothing to report yet, so the poll keeps waiting and
// a caller that gives up gets its own cancellation back rather than a
// snapshot it could act on.
func TestWaitForYieldRequestStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, _, done := countedBoard(t, state)
	defer done()

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	_, err := client.WaitForYieldRequest(ctx, testToken(state))
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("a caller that gave up = %v", err)
	}
}
