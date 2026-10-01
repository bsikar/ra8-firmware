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

// Acknowledging a grant, reporting a durable generation and waiting for a
// yield are the three calls that loop: each one re-reads the board, decides
// whether it may still speak, and either retries or hands the reason back.
// These hold the answers that end the loop. The ordinary round trips are
// already held by agent_control_test.go and yield_test.go.

// grantPendingBoard is a board that granted a lease the agent has not yet
// acknowledged, which is the only state an acknowledgement is about.
func grantPendingBoard(t *testing.T) board.Snapshot {
	t.Helper()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	return transition(t, state, board.Enqueue{Actor: "human", Waiter: board.Waiter{
		ID: testRequestID, LeaseID: testLeaseID, Holder: "human", Class: board.ClassHuman,
		Reason: "test", Duration: time.Minute,
	}})
}

// awaitingRecovery returns the same board in each of the three phases that
// mean a person has to clear the fixture before anything else may run.
func awaitingRecovery(t *testing.T, state board.Snapshot) map[string]board.Snapshot {
	t.Helper()
	required := transition(t, state, board.AgentUnavailable{Actor: "operator", Reason: "agent gone"})
	return map[string]board.Snapshot{
		"quarantined":       transition(t, state, board.Quarantine{Actor: "operator", Reason: "fixture unsafe"}),
		"recovery required": required,
		"recovering":        transition(t, required, board.BeginRecovery{Actor: "operator", PlanID: "plan-1", Reason: "reflash"}),
	}
}

// conflictingBoard serves the snapshot, refuses the first command with a
// version conflict, and applies the second. It counts the commands so a test
// can prove the client retried rather than gave up or gave up quietly.
func conflictingBoard(t *testing.T, state *board.Snapshot, apply func(board.Snapshot) board.Snapshot) (*Client, *atomic.Int64, func()) {
	t.Helper()
	var commands atomic.Int64
	client, done := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			jsonResponse(w, http.StatusOK, *state)
			return
		}
		if commands.Add(1) == 1 {
			jsonResponse(w, http.StatusConflict, map[string]any{
				"code": "conflict", "detail": "expected version is stale", "retryable": true,
			})
			return
		}
		*state = apply(*state)
		jsonResponse(w, http.StatusOK, commandResponse{Snapshot: *state})
	})
	return client, &commands, done
}

func TestAcknowledgeGrantHandsBackAnUnreachableServer(t *testing.T) {
	state := grantPendingBoard(t)
	client, _, done := countedBoard(t, state)
	done()

	_, err := client.AcknowledgeGrant(context.Background(), testToken(state))
	if err == nil {
		t.Fatal("an unreachable server was answered with a snapshot")
	}
	if errors.Is(err, ErrStaleLease) || errors.Is(err, ErrRecoveryRequired) {
		t.Fatalf("a transport failure was reported as a board decision: %v", err)
	}
}

// A board awaiting recovery is not a stale lease: the agent must stop and
// wait for a person, not re-queue and try to take the board again.
func TestAcknowledgeGrantRefusesABoardAwaitingRecovery(t *testing.T) {
	for name, state := range awaitingRecovery(t, grantPendingBoard(t)) {
		client, commands, done := countedBoard(t, state)
		if _, err := client.AcknowledgeGrant(context.Background(), testToken(state)); !errors.Is(err, ErrRecoveryRequired) {
			t.Fatalf("%s = %v", name, err)
		}
		if commands.Load() != 0 {
			t.Fatalf("%s was acknowledged anyway", name)
		}
		done()
	}
}

// A version conflict means another writer moved the board between the read
// and the command, so the acknowledgement is re-read and re-sent rather than
// reported to a caller that can do nothing about it.
func TestAcknowledgeGrantRetriesAVersionConflict(t *testing.T) {
	state := grantPendingBoard(t)
	token := testToken(state)
	client, commands, done := conflictingBoard(t, &state, func(current board.Snapshot) board.Snapshot {
		return transition(t, current, board.AcknowledgeGrant{Actor: "board-agent",
			LeaseID: token.LeaseID, Generation: token.Generation, InstalledGeneration: token.Generation})
	})
	defer done()

	result, err := client.AcknowledgeGrant(context.Background(), token)
	if err != nil {
		t.Fatalf("a version conflict was not retried: %v", err)
	}
	if result.Phase != board.Active || result.AgentHighWater != token.Generation {
		t.Fatalf("snapshot = %+v", result)
	}
	if commands.Load() != 2 {
		t.Fatalf("%d acknowledgements sent, expected a refused one and a retry", commands.Load())
	}
}

func TestObserveAgentGenerationHandsBackAnUnreachableServer(t *testing.T) {
	client, _, done := countedBoard(t, activeBoard(t))
	done()

	if _, err := client.ObserveAgentGeneration(context.Background(), "ek-ra8d2", 3); err == nil {
		t.Fatal("an unreachable server was answered with a snapshot")
	}
}

// The durable high-water report is the one call that must land: it is how a
// restored database learns it is behind the agent, so a conflict is retried.
func TestObserveAgentGenerationRetriesAVersionConflict(t *testing.T) {
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	client, commands, done := conflictingBoard(t, &state, func(current board.Snapshot) board.Snapshot {
		return transition(t, current, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: 3})
	})
	defer done()

	result, err := client.ObserveAgentGeneration(context.Background(), "ek-ra8d2", 3)
	if err != nil {
		t.Fatalf("a version conflict was not retried: %v", err)
	}
	if result.Phase != board.Quarantined || result.AgentHighWater != 3 {
		t.Fatalf("snapshot = %+v", result)
	}
	if commands.Load() != 2 {
		t.Fatalf("%d reports sent, expected a refused one and a retry", commands.Load())
	}
}

// A holder waiting to be asked to yield has to hear about recovery too: the
// board it is holding is the one a person is about to take apart.
func TestWaitForYieldRequestRefusesABoardAwaitingRecovery(t *testing.T) {
	active := activeBoard(t)
	token := testToken(active)
	for name, state := range awaitingRecovery(t, active) {
		client, _, done := countedBoard(t, state)
		if _, err := client.WaitForYieldRequest(context.Background(), token); !errors.Is(err, ErrRecoveryRequired) {
			t.Fatalf("%s = %v", name, err)
		}
		done()
	}
}

// A lease that is still only pending is not one this holder can be asked to
// yield, so the wait ends rather than polling a board it does not hold.
func TestWaitForYieldRequestRefusesAPhaseThisHolderCannotBeIn(t *testing.T) {
	state := grantPendingBoard(t)
	client, _, done := countedBoard(t, state)
	defer done()

	if _, err := client.WaitForYieldRequest(context.Background(), testToken(state)); !errors.Is(err, ErrStaleLease) {
		t.Fatalf("a pending grant was waited on: %v", err)
	}
}

// The wait is the caller's to end. An active board never resolves on its own,
// so a caller that stops waiting is answered with its own reason.
func TestWaitForYieldRequestStopsWhenTheCallerStopsWaiting(t *testing.T) {
	state := activeBoard(t)
	client, _, done := countedBoard(t, state)
	defer done()
	client.poll = 2 * time.Second

	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	started := time.Now()
	if _, err := client.WaitForYieldRequest(ctx, testToken(state)); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("the wait did not end with the caller's reason: %v", err)
	}
	if waited := time.Since(started); waited > time.Second {
		t.Fatalf("the wait outlived the caller by %v", waited)
	}
}

// A client built without a poll interval still polls. Answering a zero
// interval with a busy loop would turn one waiting agent into a load test.
func TestAPollWithoutAnIntervalStillWaits(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	started := time.Now()
	if err := waitPoll(ctx, 0); !errors.Is(err, context.Canceled) {
		t.Fatalf("poll without an interval = %v", err)
	}
	if waited := time.Since(started); waited > 500*time.Millisecond {
		t.Fatalf("a cancelled poll waited %v for its timer", waited)
	}
}

// The conflict backoff is the retry loop's only pause, so a caller that has
// given up is not held for it.
func TestAConflictBackoffEndsWithTheCaller(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := waitConflict(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("conflict backoff = %v", err)
	}
}
