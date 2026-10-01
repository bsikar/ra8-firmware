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

// reconcileControl answers Status from a snapshot the test owns and records
// every generation the agent reports back, which is how the quarantine paths
// below are told apart from an acknowledgement.
type reconcileControl struct {
	snapshot   board.Snapshot
	statusFail error
	ackFail    error
	acks       int
	observed   []uint64
}

func (c *reconcileControl) Status(context.Context, string) (board.Snapshot, error) {
	if c.statusFail != nil {
		return board.Snapshot{}, c.statusFail
	}
	return c.snapshot, nil
}

func (c *reconcileControl) AcknowledgeGrant(context.Context, boardclient.LeaseToken) (board.Snapshot, error) {
	c.acks++
	if c.ackFail != nil {
		return board.Snapshot{}, c.ackFail
	}
	return c.snapshot, nil
}

func (c *reconcileControl) ObserveAgentGeneration(_ context.Context, _ string, highWater uint64) (board.Snapshot, error) {
	c.observed = append(c.observed, highWater)
	return c.snapshot, nil
}

// unadvanceableGeneration is a durable store that can be read but not written,
// which is the shape of a state file on a full or read-only filesystem.
type unadvanceableGeneration struct {
	value       uint64
	advanceFail error
}

func (h *unadvanceableGeneration) Load() (uint64, error) { return h.value, nil }

func (h *unadvanceableGeneration) Advance(next uint64) error {
	if h.advanceFail != nil {
		return h.advanceFail
	}
	if next > h.value {
		h.value = next
	}
	return nil
}

func reconciling(t *testing.T, control ControlClient, store HighWaterStore) *Agent {
	t.Helper()
	agent, err := New("ek-ra8d2", control, store, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	agent.clock = time.Now
	return agent
}

// A pending grant is one the agent has not installed yet, so the server's view
// of what this agent holds stays behind the generation being granted.
func grantPendingSnapshot(generation uint64, leaseGeneration uint64, expiry time.Time) board.Snapshot {
	snapshot := grantedSnapshot(generation, 1, expiry)
	snapshot.Phase = board.GrantPending
	snapshot.AgentHighWater = 0
	snapshot.Lease.Generation = leaseGeneration
	return snapshot
}

// A server this agent cannot reach is not a board in a known state, so the
// transport failure is handed back rather than turned into a snapshot.
func TestReconcileHandsBackAServerItCannotReach(t *testing.T) {
	unreachable := errors.New("board control plane is unreachable")
	agent := reconciling(t, &reconcileControl{statusFail: unreachable}, &heldGeneration{})
	snapshot, err := agent.Reconcile(context.Background())
	if !errors.Is(err, unreachable) {
		t.Fatalf("err = %v, want the transport failure", err)
	}
	if snapshot.BoardID != "" {
		t.Fatalf("a snapshot was invented for an unreachable server: %+v", snapshot)
	}
}

// A server claiming this agent already installed a generation it has no
// durable record of is reporting authority the agent does not hold. The agent
// reports what it actually has instead of accepting the claim.
func TestReconcileQuarantinesAServerClaimAheadOfLocalState(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()
	claimed := grantedSnapshot(4, 1, expiry)
	control := &reconcileControl{snapshot: claimed}
	agent := reconciling(t, control, &heldGeneration{value: 3})

	if _, err := agent.Reconcile(context.Background()); err != nil {
		t.Fatalf("reconcile = %v", err)
	}
	if len(control.observed) != 1 || control.observed[0] != 3 {
		t.Fatalf("observed = %v, want the durable generation 3 reported once", control.observed)
	}
	if control.acks != 0 {
		t.Fatal("a claim ahead of local state was acknowledged")
	}
	agent.fenceMu.Lock()
	fence := agent.fence
	agent.fenceMu.Unlock()
	if fence != nil {
		t.Fatal("a quarantined snapshot still seeded a deadline fence")
	}
}

// A pending grant whose lease does not match the generation it is being
// granted under is not a grant this agent can install, and installing the
// wrong one would hand it the board under a lease someone else holds.
func TestReconcileRefusesAPendingGrantWhoseLeaseDoesNotMatch(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()

	noLease := grantPendingSnapshot(4, 4, expiry)
	noLease.Lease = nil

	for name, snapshot := range map[string]board.Snapshot{
		"a pending grant with no lease":       noLease,
		"a lease behind its own generation":   grantPendingSnapshot(4, 3, expiry),
		"a lease ahead of its own generation": grantPendingSnapshot(4, 5, expiry),
	} {
		control := &reconcileControl{snapshot: snapshot}
		agent := reconciling(t, control, &heldGeneration{value: 4})
		if _, err := agent.Reconcile(context.Background()); !errors.Is(err, boardclient.ErrStaleLease) {
			t.Errorf("%s: err = %v, want ErrStaleLease", name, err)
		}
		if control.acks != 0 || len(control.observed) != 0 {
			t.Errorf("%s: the server was told something: acks=%d observed=%v", name, control.acks, control.observed)
		}
	}
}

// The generation is persisted before the grant is acknowledged, so a state
// file that cannot be written stops the acknowledgement. Acknowledging first
// would leave the agent holding a board it has no durable record of.
func TestReconcileWillNotAcknowledgeAGrantItCannotPersist(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()
	control := &reconcileControl{snapshot: grantPendingSnapshot(5, 5, expiry)}
	unwritable := errors.New("state file is read-only")
	agent := reconciling(t, control, &unadvanceableGeneration{value: 4, advanceFail: unwritable})

	_, err := agent.Reconcile(context.Background())
	if !errors.Is(err, unwritable) {
		t.Fatalf("err = %v, want the write failure", err)
	}
	if !strings.Contains(err.Error(), "persist board generation before grant acknowledgement") {
		t.Fatalf("err does not say what it was doing: %v", err)
	}
	if control.acks != 0 {
		t.Fatal("the grant was acknowledged without a durable generation")
	}
}

// A refused acknowledgement is handed back as it happened. The durable
// generation stays advanced, because the agent really did commit to it before
// asking, and a later reconciliation has to see that.
func TestReconcileHandsBackARefusedAcknowledgement(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()
	refused := errors.New("grant acknowledgement refused")
	control := &reconcileControl{snapshot: grantPendingSnapshot(5, 5, expiry), ackFail: refused}
	store := &heldGeneration{value: 4}
	agent := reconciling(t, control, store)

	if _, err := agent.Reconcile(context.Background()); !errors.Is(err, refused) {
		t.Fatalf("err = %v, want the refusal", err)
	}
	if control.acks != 1 {
		t.Fatalf("acks = %d, want one attempt", control.acks)
	}
	if held, err := store.Load(); err != nil || held != 5 {
		t.Fatalf("durable generation = %d (err %v), want 5 to stay committed", held, err)
	}
}

// An active lease from a generation other than the one this agent installed is
// quarantined rather than run: the board is live under authority this agent
// cannot prove it holds.
func TestReconcileQuarantinesAnActiveLeaseFromAnotherGeneration(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()
	ahead := grantedSnapshot(5, 1, expiry)
	ahead.AgentHighWater = 4
	ahead.Lease.Generation = 5
	control := &reconcileControl{snapshot: ahead}
	agent := reconciling(t, control, &heldGeneration{value: 4})

	if _, err := agent.Reconcile(context.Background()); err != nil {
		t.Fatalf("reconcile = %v", err)
	}
	if len(control.observed) != 1 || control.observed[0] != 4 {
		t.Fatalf("observed = %v, want the durable generation 4 reported once", control.observed)
	}
	agent.fenceMu.Lock()
	fence := agent.fence
	agent.fenceMu.Unlock()
	if fence != nil {
		t.Fatal("an active lease from another generation still seeded a fence")
	}
}

// A board that is no longer leased takes the local fence with it. Keeping a
// fence for a lease that has ended is exactly the widened authority the fence
// exists to prevent.
func TestReconcileClearsTheFenceWhenTheBoardIsNoLongerLeased(t *testing.T) {
	expiry := time.Now().Add(time.Hour).UTC()
	control := &reconcileControl{snapshot: grantedSnapshot(4, 1, expiry)}
	agent := reconciling(t, control, &heldGeneration{value: 4})

	if _, err := agent.Reconcile(context.Background()); err != nil {
		t.Fatalf("the active reconcile failed: %v", err)
	}
	agent.fenceMu.Lock()
	seeded := agent.fence
	agent.fenceMu.Unlock()
	if seeded == nil || seeded.Generation != 4 {
		t.Fatalf("an active installed grant did not seed a fence: %+v", seeded)
	}

	released := board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready, Generation: 4, AgentHighWater: 4}
	control.snapshot = released
	snapshot, err := agent.Reconcile(context.Background())
	if err != nil {
		t.Fatalf("the released reconcile failed: %v", err)
	}
	if snapshot.Phase != board.Ready {
		t.Fatalf("snapshot = %+v, want the released board", snapshot)
	}
	agent.fenceMu.Lock()
	cleared := agent.fence
	agent.fenceMu.Unlock()
	if cleared != nil {
		t.Fatalf("the fence outlived the lease: %+v", cleared)
	}
	if len(control.observed) != 0 {
		t.Fatalf("a released board was quarantined: observed = %v", control.observed)
	}
}
