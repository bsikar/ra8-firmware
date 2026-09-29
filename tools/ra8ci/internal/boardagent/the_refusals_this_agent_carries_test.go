// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// stagedControlClient answers with exactly what a test staged, so a refusal
// the agent must CARRY can be placed at one specific door rather than
// produced by replaying the whole board state machine.
type stagedControlClient struct {
	status       board.Snapshot
	statusErr    error
	acknowledged board.Snapshot
	observed     int
}

func (c *stagedControlClient) Status(context.Context, string) (board.Snapshot, error) {
	return c.status, c.statusErr
}

func (c *stagedControlClient) AcknowledgeGrant(context.Context, boardclient.LeaseToken) (board.Snapshot, error) {
	return c.acknowledged, nil
}

func (c *stagedControlClient) ObserveAgentGeneration(_ context.Context, _ string, _ uint64) (board.Snapshot, error) {
	c.observed++
	return c.status, nil
}

type fixedHighWater struct{ value uint64 }

func (s fixedHighWater) Load() (uint64, error) { return s.value, nil }
func (s fixedHighWater) Advance(uint64) error  { return nil }

type refusingHighWater struct{}

func (refusingHighWater) Load() (uint64, error) { return 0, ErrUnsafeState }
func (refusingHighWater) Advance(uint64) error  { return ErrUnsafeState }

// stagedBoard returns one board at the two phases these tests need: the grant
// pending on the server, and that same grant acknowledged and active.
func stagedBoard(t *testing.T) (pending, active board.Snapshot, serverNow time.Time) {
	t.Helper()
	serverNow = time.Now().UTC()
	state, err := board.New("ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	waiter := board.Waiter{ID: "01996f90-3415-7cfe-8ff1-600058131afd",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Holder: "agent",
		Class: board.ClassAI, Reason: "HIL segment", Duration: time.Minute}
	pending, _, err = board.Apply(state, board.Enqueue{Actor: "agent", Waiter: waiter}, serverNow)
	if err != nil {
		t.Fatal(err)
	}
	if pending.Lease == nil {
		t.Fatal("enqueue produced no lease to acknowledge")
	}
	active, _, err = board.Apply(pending, board.AcknowledgeGrant{Actor: "board-agent",
		LeaseID: pending.Lease.ID, Generation: pending.Lease.Generation,
		InstalledGeneration: pending.Lease.Generation}, serverNow)
	if err != nil {
		t.Fatal(err)
	}
	return pending, active, serverNow
}

// withExpiredLease copies a snapshot onto a lease whose deadline has already
// passed, which is what a fence refuses to seed.
func withExpiredLease(snapshot board.Snapshot, expiry time.Time) board.Snapshot {
	lease := *snapshot.Lease
	lease.ExpiresAt = expiry
	snapshot.Lease = &lease
	return snapshot
}

func stagedAgent(t *testing.T, client ControlClient, store HighWaterStore, localNow time.Time) *Agent {
	t.Helper()
	agent, err := New("ek-ra8d2", client, store, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	agent.clock = func() time.Time { return localNow }
	return agent
}

func TestCanStartSegmentCarriesTheServerRefusalItMet(t *testing.T) {
	// The server's own failure reaches the caller unwrapped: a segment is not
	// refused as unauthorized when the truth is that nobody was asked.
	unreachable := errors.New("control plane unreachable")
	_, active, _ := stagedBoard(t)
	control := &stagedControlClient{statusErr: unreachable}
	agent := stagedAgent(t, control, fixedHighWater{value: active.Generation}, time.Now())
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: active.Lease.WaiterID,
		LeaseID: active.Lease.ID, Generation: active.Generation, ExpiresAt: active.Lease.ExpiresAt}
	if err := agent.CanStartSegment(context.Background(), token, 5*time.Second, 3*time.Second); !errors.Is(err, unreachable) {
		t.Fatalf("error = %v", err)
	}
}

func TestCanStartSegmentRefusesWhenTheDurableGenerationIsUnreadable(t *testing.T) {
	// A durable generation that cannot be READ is not a generation mismatch.
	// The refusal names the read, so an operator looks at the state file
	// rather than at the server's lease.
	_, active, _ := stagedBoard(t)
	control := &stagedControlClient{status: active}
	agent := stagedAgent(t, control, refusingHighWater{}, time.Now())
	token := boardclient.LeaseToken{BoardID: "ek-ra8d2", RequestID: active.Lease.WaiterID,
		LeaseID: active.Lease.ID, Generation: active.Generation, ExpiresAt: active.Lease.ExpiresAt}
	err := agent.CanStartSegment(context.Background(), token, 5*time.Second, 3*time.Second)
	if !errors.Is(err, ErrUnsafeState) || !strings.Contains(err.Error(), "read durable board generation") {
		t.Fatalf("error = %v", err)
	}
}

func TestReconcileCarriesAFenceRefusalAfterTheGrantWasAcknowledged(t *testing.T) {
	// The acknowledgement already happened on the server, so the snapshot it
	// produced is handed back WITH the fence refusal. Dropping it would leave
	// the caller unable to see the generation it is now bound to.
	pending, active, serverNow := stagedBoard(t)
	localNow := time.Now()
	control := &stagedControlClient{status: pending,
		acknowledged: withExpiredLease(active, serverNow.Add(-time.Hour))}
	agent := stagedAgent(t, control, fixedHighWater{value: pending.Generation}, localNow)
	snapshot, err := agent.Reconcile(context.Background())
	if err == nil {
		t.Fatal("an expired lease seeded a deadline fence")
	}
	if snapshot.Generation != active.Generation || snapshot.Lease == nil {
		t.Fatalf("the acknowledged snapshot was dropped: %+v", snapshot)
	}
	if control.observed != 0 {
		t.Fatalf("generation was re-observed %d times", control.observed)
	}
}

func TestReconcileCarriesAFenceRefusalOnAnActiveLease(t *testing.T) {
	_, active, serverNow := stagedBoard(t)
	localNow := time.Now()
	expired := withExpiredLease(active, serverNow.Add(-time.Hour))
	control := &stagedControlClient{status: expired}
	agent := stagedAgent(t, control, fixedHighWater{value: active.Generation}, localNow)
	snapshot, err := agent.Reconcile(context.Background())
	if err == nil {
		t.Fatal("an expired active lease seeded a deadline fence")
	}
	if snapshot.Generation != active.Generation {
		t.Fatalf("the observed snapshot was dropped: %+v", snapshot)
	}
}
