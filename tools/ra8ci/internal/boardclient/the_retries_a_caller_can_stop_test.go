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
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Three commands read the board, decide, then send. Each retries a version
// conflict, because a conflict means the board moved rather than that the
// caller is wrong, and each must stop retrying the moment its caller has
// given up. A loop that outlives its context is how an abandoned agent keeps
// a board looking busy.

// conflictingForever serves the snapshot to every read and answers every
// command with a version conflict, cancelling the caller's context as it does
// so, which is the caller giving up mid-retry.
func conflictingForever(t *testing.T, state board.Snapshot) (*Client, context.Context, *atomic.Int64) {
	t.Helper()
	var commands atomic.Int64
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		commands.Add(1)
		cancel()
		jsonResponse(w, http.StatusConflict, map[string]any{
			"code": "conflict", "detail": "expected version is stale", "retryable": true,
		})
	})
	t.Cleanup(done)
	return client, ctx, &commands
}

// refusing serves the snapshot and fails every command outright.
func refusing(t *testing.T, state board.Snapshot) (*Client, *atomic.Int64) {
	t.Helper()
	var commands atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		commands.Add(1)
		jsonResponse(w, http.StatusInternalServerError, map[string]any{"code": "internal", "detail": "boom"})
	})
	t.Cleanup(done)
	return client, &commands
}

// A server failure is not a conflict, so the beat is handed back rather than
// repeated. Retrying here would beat at a failing server on the caller's
// behalf while telling it nothing.
func TestAFailedBeatIsHandedBackRatherThanRepeated(t *testing.T) {
	state := activeBoard(t)
	client, commands := refusing(t, state)

	_, _, err := client.Heartbeat(context.Background(), testToken(state))
	if err == nil {
		t.Fatal("a failing server was reported as a successful beat")
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d beats, want the failure handed back after one", got)
	}
}

// A caller that gives up mid-conflict is not beaten on behalf of.
func TestBeatingStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	_, _, err := client.Heartbeat(ctx, testToken(state))
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned beat to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d beats after the caller gave up", got)
	}
}

// Recovery cannot be started against a board whose current state could not be
// read. Sending the plan anyway would start a reviewed hardware sequence
// without knowing the phase it is starting from.
func TestRecoveryIsNotStartedAgainstABoardThatCannotBeRead(t *testing.T) {
	planID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	client, done := testClient(t, func(w http.ResponseWriter, _ *http.Request) {
		jsonResponse(w, http.StatusInternalServerError, map[string]any{"code": "internal", "detail": "boom"})
	})
	t.Cleanup(done)

	_, err = client.StartRecovery(context.Background(), "ek-ra8d2", planID, "bring it back")
	if err == nil {
		t.Fatal("recovery was started against an unreadable board")
	}
	if errors.Is(err, ErrNoRecoveryPending) {
		t.Fatalf("an unreadable board was reported as having nothing to recover: %v", err)
	}
}

// The same giving-up rule, on the recovery path.
func TestStartingRecoveryStopsWhenTheCallerGivesUp(t *testing.T) {
	planID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	state := recoveringBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	_, err = client.StartRecovery(ctx, state.BoardID, planID, "bring it back")
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned start to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d starts after the caller gave up", got)
	}
}

// A lease that is not in service cannot be extended, and that is answered as
// a stale lease before anything is sent. A granted lease nobody has taken up
// has an expiry, which is exactly why it reads as extendable if the phase is
// not checked.
func TestALeaseThatIsNotInServiceCannotBeExtended(t *testing.T) {
	state := grantPendingBoard(t)
	client, commands := refusing(t, state)

	_, err := client.Extend(context.Background(), testToken(state), state.Lease.ExpiresAt.Add(time.Minute), "a little longer")
	if !errors.Is(err, ErrStaleLease) {
		t.Fatalf("answered %v, want a lease that is not in service refused", err)
	}
	if got := commands.Load(); got != 0 {
		t.Fatalf("sent %d extensions for a lease that is not in service", got)
	}
}

// And the same giving-up rule on the extension path.
func TestExtendingStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	_, err := client.Extend(ctx, testToken(state), state.Lease.ExpiresAt.Add(time.Minute), "a little longer")
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned extension to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d extensions after the caller gave up", got)
	}
}
