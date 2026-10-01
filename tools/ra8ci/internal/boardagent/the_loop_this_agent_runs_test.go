// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// Run is the loop a service manager actually starts, and the deadline fence is
// the local authority it maintains while it runs. These tests hold what makes
// the loop exit, and the fence's one rule: authority is never widened by
// anything the server says, only narrowed or re-seeded from a fresh grant.

// scriptedControl answers Status from a snapshot the test owns, counts the
// observations, and can fail on demand.
type scriptedControl struct {
	mu       sync.Mutex
	snapshot board.Snapshot
	failure  error
	calls    atomic.Int64
}

func (c *scriptedControl) Status(context.Context, string) (board.Snapshot, error) {
	c.calls.Add(1)
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.failure != nil {
		return board.Snapshot{}, c.failure
	}
	return c.snapshot, nil
}

func (c *scriptedControl) AcknowledgeGrant(context.Context, boardclient.LeaseToken) (board.Snapshot, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.snapshot, nil
}

func (c *scriptedControl) ObserveAgentGeneration(context.Context, string, uint64) (board.Snapshot, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.snapshot, nil
}

// heldGeneration is a durable high-water store kept in memory.
type heldGeneration struct {
	mu       sync.Mutex
	value    uint64
	loadFail error
}

func (h *heldGeneration) Load() (uint64, error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.loadFail != nil {
		return 0, h.loadFail
	}
	return h.value, nil
}

func (h *heldGeneration) Advance(next uint64) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	if next > h.value {
		h.value = next
	}
	return nil
}

// quietBoard is a ready board with no lease: Reconcile has nothing to install,
// which is what makes it a clean fixture for the loop itself.
func quietBoard() board.Snapshot {
	return board.Snapshot{BoardID: "ek-ra8d2", Phase: board.Ready}
}

func runnableAgent(t *testing.T, control ControlClient, interval time.Duration) *Agent {
	t.Helper()
	agent, err := New("ek-ra8d2", control, &heldGeneration{}, interval)
	if err != nil {
		t.Fatal(err)
	}
	return agent
}

func TestRunRefusesAnAgentItCannotDrive(t *testing.T) {
	var absent *Agent
	if err := absent.Run(context.Background()); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("nil agent = %v", err)
	}
	live := runnableAgent(t, &scriptedControl{snapshot: quietBoard()}, time.Second)
	if err := live.Run(nil); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("nil context = %v", err)
	}
	// A zero-value Agent has no segment gate, so it can never serialize
	// hardware access: it must be refused rather than run unguarded.
	if err := (&Agent{}).Run(context.Background()); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("agent with no segment gate = %v", err)
	}
}

func TestRunReconcilesImmediatelyAndThenOnItsInterval(t *testing.T) {
	control := &scriptedControl{snapshot: quietBoard()}
	agent := runnableAgent(t, control, 250*time.Millisecond)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- agent.Run(ctx) }()

	deadline := time.After(3 * time.Second)
	for control.calls.Load() < 3 {
		select {
		case <-deadline:
			t.Fatalf("only %d reconciliation(s) in three seconds", control.calls.Load())
		case err := <-done:
			t.Fatalf("the loop exited early: %v", err)
		case <-time.After(10 * time.Millisecond):
		}
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("a cancelled loop is not a failure: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("the loop did not return after its context was cancelled")
	}
}

// Any transport or state error exits for a service-manager restart rather than
// being swallowed: the agent must not keep looping against a server it cannot
// read, since no new generation may be assumed while disconnected.
func TestRunExitsWithTheErrorThatEndedIt(t *testing.T) {
	refused := errors.New("control plane unreachable")
	control := &scriptedControl{snapshot: quietBoard(), failure: refused}
	agent := runnableAgent(t, control, 250*time.Millisecond)
	err := agent.Run(context.Background())
	if !errors.Is(err, refused) {
		t.Fatalf("Run = %v, want the transport error", err)
	}
	if control.calls.Load() != 1 {
		t.Fatalf("the loop kept going after a failure: %d calls", control.calls.Load())
	}
}

// The same shape of failure raised BY cancellation is not an error: the
// service manager asked for the stop, so Run reports a clean exit and no
// restart-worthy error escapes. A failing server under a cancelled context is
// the sharpest case, since the cancellation must win over the failure.
func TestARunCancelledBeforeItStartsExitsCleanly(t *testing.T) {
	for name, control := range map[string]*scriptedControl{
		"a quiet board":    {snapshot: quietBoard()},
		"a failing server": {snapshot: quietBoard(), failure: errors.New("control plane unreachable")},
	} {
		agent := runnableAgent(t, control, time.Second)
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		if err := agent.Run(ctx); err != nil {
			t.Fatalf("%s: cancelled run = %v", name, err)
		}
		// Whether the gate or the server answered first is a race the
		// runtime owns; what must hold is that the loop stopped at once.
		if calls := control.calls.Load(); calls > 1 {
			t.Fatalf("%s: a cancelled run kept reconciling: %d calls", name, calls)
		}
	}
}

// The segment gate is what keeps two operations off one board at once. A
// caller that cannot take it waits, and a cancelled caller leaves rather than
// waiting forever.
func TestTheSegmentGateAdmitsOneCallerAndReleasesOnCancellation(t *testing.T) {
	agent := runnableAgent(t, &scriptedControl{snapshot: quietBoard()}, time.Second)
	if err := agent.enterSegment(context.Background()); err != nil {
		t.Fatalf("first caller refused: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := agent.enterSegment(ctx); !errors.Is(err, context.Canceled) {
		t.Fatalf("second caller = %v, want context.Canceled", err)
	}
	waiting := make(chan error, 1)
	go func() { waiting <- agent.enterSegment(context.Background()) }()
	select {
	case err := <-waiting:
		t.Fatalf("the gate admitted a second caller: %v", err)
	case <-time.After(100 * time.Millisecond):
	}
	agent.leaveSegment()
	select {
	case err := <-waiting:
		if err != nil {
			t.Fatalf("the waiting caller was refused after release: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("the gate was never released")
	}
	agent.leaveSegment()
	if err := agent.enterSegment(nil); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("nil context = %v", err)
	}
}

// grantedSnapshot is an installed, active lease: the only shape from which a
// deadline fence may be seeded.
func grantedSnapshot(generation, deadlineVersion uint64, expiry time.Time) board.Snapshot {
	return board.Snapshot{
		BoardID:        "ek-ra8d2",
		Phase:          board.Active,
		Generation:     generation,
		AgentHighWater: generation,
		Lease: &board.Lease{
			ID: "01996f90-3415-7cfe-8ff1-600058131afe", WaiterID: "01996f90-3415-7cfe-8ff1-600058131afd",
			Holder: "agent", Class: board.ClassAI, Reason: "HIL",
			Generation: generation, DeadlineVersion: deadlineVersion, ExpiresAt: expiry,
		},
	}
}

func fencedAgent(t *testing.T, now time.Time) *Agent {
	t.Helper()
	agent := runnableAgent(t, &scriptedControl{snapshot: quietBoard()}, time.Second)
	agent.clock = func() time.Time { return now }
	return agent
}

func TestADeadlineFenceNeedsAnActiveInstalledGrant(t *testing.T) {
	now := time.Now()
	expiry := now.Add(time.Hour).UTC()
	granted := grantedSnapshot(4, 1, expiry)
	noLease := granted
	noLease.Lease = nil
	draining := grantedSnapshot(4, 1, expiry)
	draining.Phase = board.Draining
	uninstalled := grantedSnapshot(4, 1, expiry)
	uninstalled.AgentHighWater = 3
	for name, snapshot := range map[string]board.Snapshot{
		"no lease":                 noLease,
		"not active":               draining,
		"generation not installed": uninstalled,
	} {
		agent := fencedAgent(t, now)
		err := agent.refreshDeadlineFence(snapshot)
		var fault *board.Error
		if !errors.As(err, &fault) || fault.Code != board.RecoveryNecessary {
			t.Fatalf("%s = %v", name, err)
		}
		if agent.fence != nil {
			t.Fatalf("%s seeded a fence anyway: %+v", name, agent.fence)
		}
	}
}

func TestAFenceIsSeededOnceAndLeftAloneAtTheSameVersion(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	snapshot := grantedSnapshot(4, 1, now.Add(time.Hour).UTC())
	if err := agent.refreshDeadlineFence(snapshot); err != nil {
		t.Fatal(err)
	}
	seeded := *agent.fence
	if seeded.Generation != 4 || seeded.Version != 1 || !seeded.Until.After(now) {
		t.Fatalf("seeded fence = %+v", seeded)
	}
	// The local deadline is conservative: it must sit strictly before the
	// server's own expiry, never at it.
	if !seeded.Until.Before(snapshot.Lease.ExpiresAt) {
		t.Fatalf("the local deadline is not conservative: %v vs %v", seeded.Until, snapshot.Lease.ExpiresAt)
	}
	if err := agent.refreshDeadlineFence(snapshot); err != nil {
		t.Fatal(err)
	}
	if *agent.fence != seeded {
		t.Fatalf("the same deadline version moved the fence: %+v -> %+v", seeded, *agent.fence)
	}
}

func TestAHigherDeadlineVersionMovesTheFenceAndAStaleOneIsRefused(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	if err := agent.refreshDeadlineFence(grantedSnapshot(4, 2, now.Add(time.Hour).UTC())); err != nil {
		t.Fatal(err)
	}
	seeded := *agent.fence
	shortened := grantedSnapshot(4, 3, now.Add(30*time.Minute).UTC())
	if err := agent.refreshDeadlineFence(shortened); err != nil {
		t.Fatal(err)
	}
	if agent.fence.Version != 3 || !agent.fence.Until.Before(seeded.Until) {
		t.Fatalf("a higher version did not shorten the fence: %+v -> %+v", seeded, *agent.fence)
	}
	held := *agent.fence
	for name, snapshot := range map[string]board.Snapshot{
		"an older deadline version": grantedSnapshot(4, 1, now.Add(time.Hour).UTC()),
		"an older generation":       grantedSnapshot(3, 9, now.Add(time.Hour).UTC()),
	} {
		err := agent.refreshDeadlineFence(snapshot)
		var fault *board.Error
		if !errors.As(err, &fault) || fault.Code != board.StaleGeneration {
			t.Fatalf("%s = %v", name, err)
		}
		if *agent.fence != held {
			t.Fatalf("%s moved the fence: %+v", name, *agent.fence)
		}
	}
}

// A NEWER generation is a fresh grant, so the fence is re-seeded rather than
// refused: that is the one path by which local authority may lengthen.
func TestANewerGenerationReseedsTheFence(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	if err := agent.refreshDeadlineFence(grantedSnapshot(4, 3, now.Add(10*time.Minute).UTC())); err != nil {
		t.Fatal(err)
	}
	short := *agent.fence
	if err := agent.refreshDeadlineFence(grantedSnapshot(5, 1, now.Add(time.Hour).UTC())); err != nil {
		t.Fatal(err)
	}
	if agent.fence.Generation != 5 || agent.fence.Version != 1 || !agent.fence.Until.After(short.Until) {
		t.Fatalf("a fresh grant did not re-seed the fence: %+v -> %+v", short, *agent.fence)
	}
}

func TestAGrantWithNoDeadlineVersionCannotSeedAFence(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	if err := agent.refreshDeadlineFence(grantedSnapshot(4, 0, now.Add(time.Hour).UTC())); err == nil {
		t.Fatal("a zero deadline version seeded a fence")
	}
	if agent.fence != nil {
		t.Fatalf("fence = %+v", agent.fence)
	}
}

// An expiry that has already passed locally cannot become authority.
func TestAnExpiredGrantCannotSeedAFence(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	if err := agent.refreshDeadlineFence(grantedSnapshot(4, 1, now.Add(-time.Minute).UTC())); err == nil {
		t.Fatal("an expired grant seeded a fence")
	}
	if agent.fence != nil {
		t.Fatalf("fence = %+v", agent.fence)
	}
}

// Clearing is what makes a lease that went away safe to replace: without it an
// older generation would be refused as stale forever.
func TestClearingTheFenceLetsAnEarlierGenerationSeedAgain(t *testing.T) {
	now := time.Now()
	agent := fencedAgent(t, now)
	if err := agent.refreshDeadlineFence(grantedSnapshot(5, 4, now.Add(time.Hour).UTC())); err != nil {
		t.Fatal(err)
	}
	if err := agent.refreshDeadlineFence(grantedSnapshot(3, 1, now.Add(time.Hour).UTC())); err == nil {
		t.Fatal("an earlier generation was accepted while the fence was held")
	}
	agent.clearDeadlineFence()
	if agent.fence != nil {
		t.Fatalf("the fence survived clearing: %+v", agent.fence)
	}
	if err := agent.refreshDeadlineFence(grantedSnapshot(3, 1, now.Add(time.Hour).UTC())); err != nil {
		t.Fatalf("a cleared fence refused a fresh grant: %v", err)
	}
	if agent.fence.Generation != 3 {
		t.Fatalf("fence = %+v", agent.fence)
	}
}

// And the production path that does the clearing: a reconciliation that finds
// no lease at all drops local authority instead of keeping the last one.
func TestReconcilingABoardWithNoLeaseDropsLocalAuthority(t *testing.T) {
	now := time.Now()
	control := &scriptedControl{snapshot: quietBoard()}
	agent := runnableAgent(t, control, time.Second)
	agent.clock = func() time.Time { return now }
	if err := agent.refreshDeadlineFence(grantedSnapshot(5, 4, now.Add(time.Hour).UTC())); err != nil {
		t.Fatal(err)
	}
	snapshot, err := agent.Reconcile(context.Background())
	if err != nil || snapshot.Phase != board.Ready {
		t.Fatalf("reconcile = %+v, %v", snapshot, err)
	}
	if agent.fence != nil {
		t.Fatalf("a board with no lease left a fence behind: %+v", agent.fence)
	}
}

func TestReconcileRefusesAnAgentItCannotDrive(t *testing.T) {
	var absent *Agent
	if _, err := absent.Reconcile(context.Background()); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("nil agent = %v", err)
	}
	live := runnableAgent(t, &scriptedControl{snapshot: quietBoard()}, time.Second)
	if _, err := live.Reconcile(nil); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("nil context = %v", err)
	}
}

// A durable state file the agent cannot read is never treated as generation
// zero: that would be a silent grant of authority it does not hold.
func TestAnUnreadableDurableGenerationStopsReconciliation(t *testing.T) {
	unreadable := errors.New("state file is unreadable")
	agent, err := New("ek-ra8d2", &scriptedControl{snapshot: quietBoard()}, &heldGeneration{loadFail: unreadable}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := agent.Reconcile(context.Background()); !errors.Is(err, unreadable) {
		t.Fatalf("reconcile = %v, want the read failure", err)
	}
}
