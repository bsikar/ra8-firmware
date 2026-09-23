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

func TestCanStartSegmentRequiresServerAndDurableFences(t *testing.T) {
	serverNow := time.Now().UTC()
	localNow := time.Now()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131afd",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Holder: "agent",
		Class: board.ClassAI, Reason: "HIL segment", Duration: time.Minute}
	state, _, err = board.Apply(state, board.Enqueue{Actor: "agent", Waiter: waiter}, serverNow)
	if err != nil {
		t.Fatal(err)
	}
	grant := state.Lease
	state, _, err = board.Apply(state, board.AcknowledgeGrant{Actor: "board-agent",
		LeaseID: grant.ID, Generation: grant.Generation, InstalledGeneration: grant.Generation}, serverNow)
	if err != nil {
		t.Fatal(err)
	}
	durable, _ := newTestHighWater(t)
	if err := durable.Advance(state.Generation); err != nil {
		t.Fatal(err)
	}
	control := &testControlClient{state: state}
	agent, err := New("ek-ra8d2", control, durable, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	agent.clock = func() time.Time { return localNow }
	token := boardclient.LeaseToken{BoardID: state.BoardID, RequestID: state.Lease.WaiterID,
		LeaseID: state.Lease.ID, Generation: state.Generation, ExpiresAt: state.Lease.ExpiresAt,
		Version: state.Version}
	if err := agent.CanStartSegment(context.Background(), token, 5*time.Second, 3*time.Second); err != nil {
		t.Fatalf("valid lease and durable fences rejected segment: %v", err)
	}
	wrongGeneration := token
	wrongGeneration.Generation++
	if err := agent.CanStartSegment(context.Background(), wrongGeneration,
		time.Second, time.Second); !board.IsCode(err, board.RecoveryNecessary) {
		t.Fatalf("non-durable generation authorized a segment: %v", err)
	}
	human := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131b01",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131b02", Holder: "human",
		Class: board.ClassHuman, Reason: "operator needs board", Duration: time.Minute}
	control.state, _, err = board.Apply(control.state, board.Enqueue{Actor: "human", Waiter: human}, serverNow)
	if err != nil {
		t.Fatal(err)
	}
	if control.state.Phase != board.YieldRequested {
		t.Fatalf("human waiter did not request cooperative yield: %s", control.state.Phase)
	}
	if err := agent.CanStartSegment(context.Background(), token,
		time.Second, time.Second); !board.IsCode(err, board.RecoveryNecessary) {
		t.Fatalf("segment started after yield request: %v", err)
	}
}
