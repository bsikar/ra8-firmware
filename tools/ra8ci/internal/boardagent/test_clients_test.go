// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type testControlClient struct {
	state            board.Snapshot
	onAcknowledge    func(boardclient.LeaseToken) error
	acknowledgements int
	observations     int
}

func (c *testControlClient) Status(context.Context, string) (board.Snapshot, error) {
	return c.state, nil
}

func (c *testControlClient) AcknowledgeGrant(_ context.Context, token boardclient.LeaseToken) (board.Snapshot, error) {
	c.acknowledgements++
	if c.onAcknowledge != nil {
		if err := c.onAcknowledge(token); err != nil {
			return board.Snapshot{}, err
		}
	}
	result, _, err := board.Apply(c.state, board.AcknowledgeGrant{Actor: "board-agent",
		LeaseID: token.LeaseID, Generation: token.Generation, InstalledGeneration: token.Generation}, time.Now().UTC())
	c.state = result
	return result, err
}

func (c *testControlClient) ObserveAgentGeneration(_ context.Context, boardID string, highWater uint64) (board.Snapshot, error) {
	c.observations++
	result, _, err := board.Apply(c.state, board.ObserveAgentGeneration{Actor: "board-agent", HighWater: highWater}, time.Now().UTC())
	c.state = result
	return result, err
}

type testSegmentControlClient struct {
	*testControlClient
	begins       int
	finishes     []string
	lastSegment  store.BoardSegment
	claimCount   int
	claimedLease string
	completed    int
	completion   store.BoardHILCompletion
}

func (c *testSegmentControlClient) BeginSegment(_ context.Context, token boardclient.LeaseToken, attemptID, key string, bound, margin time.Duration) (store.BoardSegment, error) {
	c.begins++
	now := time.Now()
	c.lastSegment = store.BoardSegment{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		BoardID: token.BoardID, LeaseID: token.LeaseID, Generation: token.Generation, AttemptID: attemptID,
		Key: key, StartedAt: now, DeadlineAt: now.Add(bound), RecoveryMarginMS: uint64(margin.Milliseconds())}
	return c.lastSegment, nil
}

func (c *testSegmentControlClient) ClaimNextHILAttempt(_ context.Context, boardID, leaseID, host string, cores int, ramBytes int64, load float64, facts json.RawMessage) (*store.BoardHILAssignment, error) {
	c.claimCount++
	c.claimedLease = leaseID
	return &store.BoardHILAssignment{Attempt: store.Attempt{ID: "01996f90-3415-7cfe-8ff1-600058131aff",
		TaskID: "01996f90-3415-7cfe-8ff1-600058131b11", AttemptNo: 1, State: "running"}}, nil
}

func (c *testSegmentControlClient) CompleteHILAttempt(_ context.Context, _ boardclient.LeaseToken,
	_ store.BoardHILAssignment, completion store.BoardHILCompletion) error {
	c.completed++
	c.completion = completion
	return nil
}

func (c *testSegmentControlClient) FinishSegment(_ context.Context, _ boardclient.LeaseToken, attemptID, id, outcome string) error {
	if id != c.lastSegment.ID {
		return boardclient.ErrStaleLease
	}
	c.finishes = append(c.finishes, outcome)
	return nil
}

func newActiveSegmentAgent(t *testing.T) (*Agent, *testSegmentControlClient, boardclient.LeaseToken) {
	t.Helper()
	now := time.Now().UTC()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131afd",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Holder: "agent",
		Class: board.ClassAI, Reason: "test HIL", Duration: time.Minute}
	state, _, err = board.Apply(state, board.Enqueue{Actor: "agent", Waiter: waiter}, now)
	if err != nil {
		t.Fatal(err)
	}
	state, _, err = board.Apply(state, board.AcknowledgeGrant{Actor: "board-agent",
		LeaseID: waiter.LeaseID, Generation: state.Generation, InstalledGeneration: state.Generation}, now)
	if err != nil {
		t.Fatal(err)
	}
	highWater, _ := newTestHighWater(t)
	if err := highWater.Advance(state.Generation); err != nil {
		t.Fatal(err)
	}
	client := &testSegmentControlClient{testControlClient: &testControlClient{state: state}}
	agent, err := New(state.BoardID, client, highWater, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	token := boardclient.LeaseToken{BoardID: state.BoardID, RequestID: waiter.ID,
		LeaseID: waiter.LeaseID, Generation: state.Generation, ExpiresAt: state.Lease.ExpiresAt,
		Version: state.Version}
	return agent, client, token
}
