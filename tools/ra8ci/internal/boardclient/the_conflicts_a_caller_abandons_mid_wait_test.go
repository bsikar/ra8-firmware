// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Ten commands read the board, decide against the version they read, and send.
// Each treats a version conflict as the board having moved rather than the
// caller being wrong, so each waits and reads again. The wait is the part that
// has to answer to the caller's context: a caller that walks away during the
// wait must not be woken into another round.
//
// The existing abandonment tests cancel the context as the conflict is being
// written, which fails the request itself, so the client never reaches its
// wait at all. These take the other half: the conflict arrives intact, and the
// caller gives up while the client is sitting in the pause between tries.

// abandonedMidWait serves the snapshot to every read and answers every command
// with a version conflict, then gives the caller up a few milliseconds later,
// inside the wait rather than before the conflict is delivered. A challenge is
// served when one is supplied, which is what carries a two-step command past
// its first exchange and into the one that conflicts.
func abandonedMidWait(t *testing.T, state board.Snapshot, challenge *store.NeutralChallenge) (*Client, context.Context, *atomic.Int64) {
	t.Helper()
	var commands atomic.Int64
	var once sync.Once
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method != http.MethodPost:
			jsonResponse(w, http.StatusOK, state)
		case challenge != nil && strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			jsonResponse(w, http.StatusOK, *challenge)
		default:
			commands.Add(1)
			once.Do(func() {
				go func() {
					time.Sleep(5 * time.Millisecond)
					cancel()
				}()
			})
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "expected version is stale", "retryable": true,
			})
		}
	})
	t.Cleanup(done)
	return client, ctx, &commands
}

// stopped is the answer every one of these must give: the caller's own
// cancellation, handed back rather than swallowed into a retry.
func stopped(t *testing.T, what string, err error, commands *atomic.Int64) {
	t.Helper()
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("%s answered %v, want the abandoned caller's own cancellation", what, err)
	}
	if got := commands.Load(); got < 1 {
		t.Fatalf("%s never reached a conflict, so the wait was never under test", what)
	}
}

// An agent generation observation is a command like any other, and a board
// whose agent has gone is exactly the board this races against.
func TestObservingAGenerationStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.ObserveAgentGeneration(ctx, state.BoardID, 7)
	stopped(t, "the observation", err, commands)
}

// A cancelled waiter that keeps retrying holds a queue position the caller no
// longer wants, which is the opposite of what the cancel was for.
func TestCancellingAWaiterStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state, ticket := queuedTicket(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	stopped(t, "the cancel", client.Cancel(ctx, ticket), commands)
}

// A checkpoint says the work is at a safe point. Once the work is gone, so is
// the claim, so the retry stops with it.
func TestCheckpointingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := yieldedBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.Checkpoint(ctx, testToken(state))
	stopped(t, "the checkpoint", err, commands)
}

// A beat from a holder that has gone away is the board looking occupied by
// nobody, which is the failure heartbeats exist to prevent.
func TestBeatingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, _, err := client.Heartbeat(ctx, testToken(state))
	stopped(t, "the beat", err, commands)
}

// Starting a reviewed hardware sequence on behalf of an operator who has gone
// is worse than not starting it.
func TestStartingRecoveryStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := recoveringBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.StartRecovery(ctx, state.BoardID, testPlanID, "bring it back")
	stopped(t, "the start", err, commands)
}

// An extension asked for by a caller that has stopped running would hold the
// board past the point anyone is using it.
func TestExtendingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.Extend(ctx, testToken(state), state.Lease.ExpiresAt.Add(time.Hour), "more time")
	stopped(t, "the extension", err, commands)
}

// Asking for a release challenge is itself a command against a version, so it
// can conflict on its own, before any receipt is produced. A caller that has
// gone stops asking rather than minting one-use challenges nobody will answer.
func TestAskingForAReleaseChallengeStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.Free(ctx, testToken(state), &receiptProducer{receipt: []byte("x")})
	stopped(t, "the challenge request", err, commands)
}

// The second half of the same command: the challenge was answered and signed,
// and the release itself conflicts. The signed receipt is spent either way, so
// stopping here is what keeps the client from spending another one.
func TestReleasingStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	challenge := releaseChallenge(state, token)
	client, ctx, commands := abandonedMidWait(t, state, &challenge)

	_, err := client.Free(ctx, token, &receiptProducer{receipt: []byte("board-agent-signature")})
	stopped(t, "the release", err, commands)
}

// The recovery arm of the same two-step exchange, first half.
func TestAskingForARecoveryChallengeStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := recoveringInProgress(t)
	client, ctx, commands := abandonedMidWait(t, state, nil)

	_, err := client.FinishRecovery(ctx, state.BoardID, &receiptProducer{receipt: []byte("x")})
	stopped(t, "the recovery challenge request", err, commands)
}

// And its second half: the completion conflicts after the challenge is signed.
func TestFinishingRecoveryStopsWhenTheCallerGivesUpMidWait(t *testing.T) {
	state := recoveringInProgress(t)
	challenge := recoveryChallenge(state)
	client, ctx, commands := abandonedMidWait(t, state, &challenge)

	_, err := client.FinishRecovery(ctx, state.BoardID, &receiptProducer{receipt: []byte("board-agent-signature")})
	stopped(t, "the completion", err, commands)
}
