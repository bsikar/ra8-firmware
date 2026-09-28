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
)

// A checkpoint is the holder saying it has reached a divisible point, so it
// is only ever sent for the lease that actually holds the board and only
// once a yield has been asked for. These hold the refusals taken before
// the command, the body it sends when it does, and its retry when the
// server says the version moved under it.

// yieldedBoard is a board an agent holds that a higher-priority human
// waiter has since asked for, built by real transitions because the client
// validates every snapshot it is handed.
func yieldedBoard(t *testing.T) board.Snapshot {
	t.Helper()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	state = transition(t, state, board.Enqueue{Actor: "agent", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "agent", Class: board.ClassAI,
		Reason: "automated HIL", Duration: time.Minute,
	}})
	state = transition(t, state, board.AcknowledgeGrant{Actor: "board-agent", LeaseID: testLeaseID,
		Generation: state.Generation, InstalledGeneration: state.Generation})
	return transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
		ID: testProofID, LeaseID: "01996f90-3415-7cfe-8ff1-600058131b00", Holder: "human",
		Class: board.ClassHuman, Reason: "manual board work", Duration: time.Minute,
	}})
}

func TestCheckpointRefusesATokenItCannotBeAbout(t *testing.T) {
	state := yieldedBoard(t)
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

	for name, bad := range map[string]LeaseToken{
		"a board ID that is not one":   noBoard,
		"a request ID that is not one": noRequest,
		"no lease ID":                  noLease,
		"no generation":                noGeneration,
	} {
		if _, err := client.Checkpoint(context.Background(), bad); !errors.Is(err, ErrInvalidRequest) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d refused checkpoints still reached the server", commands.Load())
	}
}

// Nobody has asked for the board, so there is nothing to hand over and the
// holder is told to carry on rather than being stopped mid-segment.
func TestCheckpointRefusesABoardNobodyAskedFor(t *testing.T) {
	state := activeBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	if _, err := client.Checkpoint(context.Background(), testToken(state)); !errors.Is(err, ErrNoYieldRequest) {
		t.Fatalf("a board nobody asked for = %v", err)
	}
	if commands.Load() != 0 {
		t.Fatal("a board nobody asked for was checkpointed at the server")
	}
}

// A token for another lease, or another generation of this one, cannot
// check the board in: the holder it names is not the holder the board has.
func TestCheckpointRefusesAnotherHolder(t *testing.T) {
	state := yieldedBoard(t)
	client, commands, done := countedBoard(t, state)
	defer done()

	otherLease := testToken(state)
	otherLease.LeaseID = "01996f90-3415-7cfe-8ff1-600058131b01"
	otherGeneration := testToken(state)
	otherGeneration.Generation = state.Lease.Generation + 1

	for name, token := range map[string]LeaseToken{
		"another lease":      otherLease,
		"another generation": otherGeneration,
	} {
		if _, err := client.Checkpoint(context.Background(), token); !errors.Is(err, ErrStaleLease) {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if commands.Load() != 0 {
		t.Fatalf("%d stale checkpoints still reached the server", commands.Load())
	}
}

// The command carries the version the client just read, so the server can
// refuse it if the board moved, plus the lease and generation it is
// checking in for.
func TestCheckpointSendsTheVersionItJustRead(t *testing.T) {
	state := yieldedBoard(t)
	token := testToken(state)
	var body struct {
		ExpectedVersion uint64 `json:"expected_version"`
		LeaseID         string `json:"lease_id"`
		Generation      uint64 `json:"generation"`
	}
	var path string
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			path = r.URL.Path
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
			return
		}
		jsonResponse(w, http.StatusOK, state)
	})
	defer done()

	snapshot, err := client.Checkpoint(context.Background(), token)
	if err != nil {
		t.Fatalf("checkpoint a yielded board: %v", err)
	}
	if snapshot.BoardID != state.BoardID {
		t.Fatalf("snapshot = %+v", snapshot)
	}
	if path != "/v1/boards/ek-ra8d2/checkpoint" {
		t.Fatalf("checkpoint went to %q", path)
	}
	if body.ExpectedVersion != state.Version || body.LeaseID != token.LeaseID || body.Generation != token.Generation {
		t.Fatalf("checkpoint body = %+v, board version %d", body, state.Version)
	}
}

// A version that moved under the client is not a failure: the board is
// read again and the checkpoint re-sent, which is what keeps a yield from
// needing the agent's help to recover.
func TestCheckpointRetriesWhenTheVersionMovedUnderIt(t *testing.T) {
	state := yieldedBoard(t)
	var posts atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		if posts.Add(1) == 1 {
			jsonResponse(w, http.StatusConflict, map[string]any{"error": "version moved"})
			return
		}
		jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
	})
	defer done()

	if _, err := client.Checkpoint(context.Background(), testToken(state)); err != nil {
		t.Fatalf("checkpoint after a conflict: %v", err)
	}
	if posts.Load() != 2 {
		t.Fatalf("the conflicted checkpoint was sent %d times", posts.Load())
	}
}

// A caller that gives up during the wait between a conflict and the retry
// gets its own cancellation back, not a checkpoint it can no longer be
// sure of.
func TestCheckpointStopsWhenTheCallerGivesUpAfterAConflict(t *testing.T) {
	state := yieldedBoard(t)
	ctx, cancel := context.WithCancel(context.Background())
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		cancel()
		jsonResponse(w, http.StatusConflict, map[string]any{"error": "version moved"})
	})
	defer done()
	defer cancel()

	if _, err := client.Checkpoint(ctx, testToken(state)); !errors.Is(err, context.Canceled) {
		t.Fatalf("a caller that gave up mid-retry = %v", err)
	}
}
