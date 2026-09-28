// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A release and a recovery completion are each two exchanges: ask for a
// one-use challenge, then submit the receipt that answers it. Both
// exchanges retry a version conflict, so both have to stop when the caller
// is gone. Neither retry loop had that pinned, and a release loop that
// outlives its caller keeps asking a board for challenges nobody will ever
// answer.

// challengedThenContested serves the board and the challenge, then
// conflicts on the command that carries the receipt, cancelling the
// caller's context as it does.
func challengedThenContested(t *testing.T, state board.Snapshot, challenge store.NeutralChallenge) (*Client, context.Context, *atomic.Int64) {
	t.Helper()
	var receipts atomic.Int64
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method != http.MethodPost:
			jsonResponse(w, http.StatusOK, state)
		case strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			jsonResponse(w, http.StatusOK, challenge)
		default:
			receipts.Add(1)
			cancel()
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "expected version is stale", "retryable": true,
			})
		}
	})
	t.Cleanup(done)
	return client, ctx, &receipts
}

// A checkpoint is the agent saying its work is at a safe point, and it
// races every other transition on the board. It stops retrying when the
// agent that would have honoured the yield is gone.
func TestCheckpointingStopsWhenTheCallerGivesUp(t *testing.T) {
	state := yieldedBoard(t)
	client, ctx, commands := conflictingForever(t, state)

	if _, err := client.Checkpoint(ctx, testToken(state)); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned checkpoint to stop", err)
	}
	if got := commands.Load(); got != 1 {
		t.Fatalf("sent %d checkpoints after the caller gave up", got)
	}
}

// Asking for a release challenge is itself a command against a version, so
// it can conflict. A caller that has gone away stops asking rather than
// minting one-use challenges nobody will answer.
func TestAskingForAReleaseChallengeStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	client, ctx, asks := conflictingForever(t, state)
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}

	if _, err := client.Free(ctx, testToken(state), producer); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned release to stop", err)
	}
	if got := asks.Load(); got != 1 {
		t.Fatalf("asked for %d challenges after the caller gave up", got)
	}
	if producer.seen != (store.NeutralChallenge{}) {
		t.Fatalf("neutralized the board for a release that was abandoned: %+v", producer.seen)
	}
}

// And once the receipt exists, the submission retries a conflict too. The
// hardware is already neutral at this point, so stopping is safe; what
// must not happen is an endless resubmission by a caller that is gone.
func TestSubmittingAReleaseReceiptStopsWhenTheCallerGivesUp(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	client, ctx, receipts := challengedThenContested(t, state, releaseChallenge(state, token))

	if _, err := client.Free(ctx, token, &receiptProducer{receipt: []byte("board-agent-signature")}); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned release to stop", err)
	}
	if got := receipts.Load(); got != 1 {
		t.Fatalf("submitted %d receipts after the caller gave up", got)
	}
}

// The recovery completion is the same two exchanges, and stops the same
// way at each of them.
func TestAskingForARecoveryChallengeStopsWhenTheCallerGivesUp(t *testing.T) {
	state := recoveringInProgress(t)
	client, ctx, asks := conflictingForever(t, state)

	if _, err := client.FinishRecovery(ctx, state.BoardID, &receiptProducer{receipt: []byte("signed")}); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned completion to stop", err)
	}
	if got := asks.Load(); got != 1 {
		t.Fatalf("asked for %d challenges after the caller gave up", got)
	}
}

func TestSubmittingARecoveryReceiptStopsWhenTheCallerGivesUp(t *testing.T) {
	state := recoveringInProgress(t)
	client, ctx, receipts := challengedThenContested(t, state, recoveryChallenge(state))

	if _, err := client.FinishRecovery(ctx, state.BoardID, &receiptProducer{receipt: []byte("signed")}); !errors.Is(err, context.Canceled) {
		t.Fatalf("answered %v, want the abandoned completion to stop", err)
	}
	if got := receipts.Load(); got != 1 {
		t.Fatalf("submitted %d receipts after the caller gave up", got)
	}
}
