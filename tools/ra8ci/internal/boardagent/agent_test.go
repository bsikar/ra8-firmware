// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
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

func TestReconcilePersistsGenerationBeforeGrantAck(t *testing.T) {
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131afd",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Holder: "agent",
		Class: board.ClassAI, Reason: "HIL", Duration: time.Minute}
	state, _, err = board.Apply(state, board.Enqueue{Actor: "agent", Waiter: waiter}, time.Now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	durable, _ := newTestHighWater(t)
	control := &testControlClient{state: state}
	control.onAcknowledge = func(token boardclient.LeaseToken) error {
		highWater, err := durable.Load()
		if err != nil || highWater != token.Generation {
			t.Fatalf("server grant acknowledged before durable state: high_water=%d err=%v", highWater, err)
		}
		return nil
	}
	agent, err := New("ek-ra8d2", control, durable, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	result, err := agent.Reconcile(context.Background())
	if err != nil || result.Phase != board.Active || control.acknowledgements != 1 {
		t.Fatalf("grant was not reconciled: snapshot=%+v acks=%d err=%v", result, control.acknowledgements, err)
	}
}

func TestReconcileQuarantinesServerRestoredBehindLocalHighWater(t *testing.T) {
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	durable, _ := newTestHighWater(t)
	if err := durable.Advance(3); err != nil {
		t.Fatal(err)
	}
	control := &testControlClient{state: state}
	agent, err := New("ek-ra8d2", control, durable, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	result, err := agent.Reconcile(context.Background())
	if err != nil || result.Phase != board.Quarantined || result.AgentHighWater != 3 ||
		control.observations != 1 || control.acknowledgements != 0 {
		t.Fatalf("restored database did not force quarantine: %+v observe=%d ack=%d err=%v",
			result, control.observations, control.acknowledgements, err)
	}
}
