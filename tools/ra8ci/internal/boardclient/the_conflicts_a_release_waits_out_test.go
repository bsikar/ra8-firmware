// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A release and a recovery completion both mint a one-use challenge and then
// submit a proof against it. Either step can lose a race with another writer,
// and the board is still held while that is true, so a conflict is waited out
// and the whole exchange retried rather than handed back as a failure. What is
// never retried is a refusal: that one is the server's answer.

func releaseChallenge(state board.Snapshot, token LeaseToken) store.NeutralChallenge {
	return store.NeutralChallenge{
		ID: testProofID, Nonce: "nonce", BoardID: state.BoardID, Purpose: "release",
		LeaseID: token.LeaseID, Generation: token.Generation, SnapshotVersion: state.Version,
		AgentHighWater: state.AgentHighWater, FixtureRevision: "rev-3",
		ProfileSHA256: "abc", RestorePolicy: "reflash",
		IssuedAt: time.Now().UTC(), ExpiresAt: time.Now().UTC().Add(30 * time.Second),
	}
}

// contested serves one board and one challenge, and refuses the first
// `conflicts` requests to the named route with 409 before answering it.
func contested(t *testing.T, state board.Snapshot, challenge store.NeutralChallenge,
	contestedRoute string, conflicts int) (*Client, func() (int, int), func()) {
	t.Helper()
	var mu sync.Mutex
	minted, submitted := 0, 0
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		defer mu.Unlock()
		conflict := func() {
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "another writer got there first", "retryable": true,
			})
		}
		switch {
		case r.Method == http.MethodGet:
			jsonResponse(w, http.StatusOK, state)
		case strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			minted++
			if contestedRoute == "challenge" && minted <= conflicts {
				conflict()
				return
			}
			jsonResponse(w, http.StatusCreated, challenge)
		default:
			submitted++
			if contestedRoute == "command" && submitted <= conflicts {
				conflict()
				return
			}
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
	return client, func() (int, int) {
		mu.Lock()
		defer mu.Unlock()
		return minted, submitted
	}, done
}

// Losing the race to mint a release challenge is not a reason to leave a
// board held: the exchange starts over against the version that won.
func TestFreeWaitsOutAConflictOnTheChallenge(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	client, counts, done := contested(t, state, releaseChallenge(state, token), "challenge", 2)
	defer done()

	snapshot, err := client.Free(context.Background(), token, producer)
	if err != nil || snapshot.BoardID != "ek-ra8d2" {
		t.Fatalf("a contested release did not finish: %+v err=%v", snapshot, err)
	}
	minted, submitted := counts()
	if minted != 3 || submitted != 1 {
		t.Fatalf("a contested release minted %d challenges and submitted %d proofs, want 3 and 1", minted, submitted)
	}
}

// Losing the race on the release itself is the same: the proof is minted
// afresh against the new version rather than replayed against the old one.
func TestFreeWaitsOutAConflictOnTheRelease(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	client, counts, done := contested(t, state, releaseChallenge(state, token), "command", 1)
	defer done()

	if _, err := client.Free(context.Background(), token, producer); err != nil {
		t.Fatalf("a contested release did not finish: %v", err)
	}
	minted, submitted := counts()
	if minted != 2 || submitted != 2 {
		t.Fatalf("a contested release minted %d challenges and submitted %d proofs, want 2 and 2", minted, submitted)
	}
}

// A conflict is waited out, and a caller who has stopped waiting is not
// kept in that loop.
func TestFreeStopsWaitingWhenTheCallerHas(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	client, counts, done := contested(t, state, releaseChallenge(state, token), "challenge", 1_000_000)
	defer done()

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := client.Free(ctx, token, producer); !errors.Is(err, context.Canceled) {
		t.Fatalf("a cancelled release = %v", err)
	}
	if _, submitted := counts(); submitted != 0 {
		t.Fatalf("a cancelled release still submitted %d proofs", submitted)
	}
}

// A refused challenge is the server's answer, not a race: it is handed back
// rather than retried until the lease runs out.
func TestFreeHandsBackARefusedChallenge(t *testing.T) {
	state := activeBoard(t)
	token := testToken(state)
	producer := &receiptProducer{receipt: []byte("board-agent-signature")}
	minted := 0
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			jsonResponse(w, http.StatusOK, state)
			return
		}
		minted++
		jsonResponse(w, http.StatusServiceUnavailable, map[string]any{
			"code": "unavailable", "detail": "the neutral box is offline", "retryable": true,
		})
	})
	defer done()

	if _, err := client.Free(context.Background(), token, producer); err == nil {
		t.Fatal("a refused challenge was read as a released board")
	}
	if minted != 1 {
		t.Fatalf("a refused challenge was asked for %d times", minted)
	}
	if producer.seen.ID != "" {
		t.Fatal("a challenge that was never served was signed anyway")
	}
}

// An unreachable server is not a stale lease: reading it as one would tell
// a holder their board is already gone.
func TestFreeHandsBackAnUnreachableServer(t *testing.T) {
	state := activeBoard(t)
	client, _, done := countedBoard(t, state)
	done()

	_, err := client.Free(context.Background(), testToken(state), &receiptProducer{receipt: []byte("x")})
	if err == nil || errors.Is(err, ErrStaleLease) {
		t.Fatalf("an unreachable server was read as board state: %v", err)
	}
}

// A recovery completion waits out the same races, and a board in recovery
// stays in recovery until one of them lands.
func TestFinishRecoveryWaitsOutAConflict(t *testing.T) {
	state := recoveringInProgress(t)
	challenge := recoveryChallenge(state)

	client, counts, done := contested(t, state, challenge, "challenge", 2)
	if _, err := client.FinishRecovery(context.Background(), "ek-ra8d2", &receiptProducer{receipt: []byte("signed")}); err != nil {
		t.Fatalf("a contested recovery completion did not finish: %v", err)
	}
	if minted, submitted := counts(); minted != 3 || submitted != 1 {
		t.Fatalf("a contested challenge minted %d and submitted %d, want 3 and 1", minted, submitted)
	}
	done()

	onSubmit, submitCounts, doneSubmit := contested(t, state, challenge, "command", 1)
	defer doneSubmit()
	if _, err := onSubmit.FinishRecovery(context.Background(), "ek-ra8d2", &receiptProducer{receipt: []byte("signed")}); err != nil {
		t.Fatalf("a contested completion did not finish: %v", err)
	}
	if minted, submitted := submitCounts(); minted != 2 || submitted != 2 {
		t.Fatalf("a contested completion minted %d and submitted %d, want 2 and 2", minted, submitted)
	}
}

// A refused challenge ends a recovery completion, and nothing is submitted
// against a challenge that was never issued.
func TestFinishRecoveryHandsBackARefusedChallenge(t *testing.T) {
	state := recoveringInProgress(t)
	submitted := 0
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet:
			jsonResponse(w, http.StatusOK, state)
		case strings.HasSuffix(r.URL.Path, "/neutral-challenge"):
			jsonResponse(w, http.StatusForbidden, map[string]any{
				"code": "forbidden", "detail": "this plan is not approved", "retryable": false,
			})
		default:
			submitted++
			jsonResponse(w, http.StatusOK, map[string]any{"snapshot": state, "events": []board.Event{}})
		}
	})
	defer done()

	if _, err := client.FinishRecovery(context.Background(), "ek-ra8d2", &receiptProducer{receipt: []byte("signed")}); err == nil {
		t.Fatal("a refused challenge was read as a completed recovery")
	}
	if submitted != 0 {
		t.Fatalf("a completion was submitted against a challenge that was never issued (%d)", submitted)
	}
}
