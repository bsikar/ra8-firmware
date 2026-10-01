// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"encoding/json"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// segmentPlane is a staged control client that also speaks the durable
// segment and HIL claim APIs, so the gate and the begin-segment refusal can
// be reached without a database behind them.
type segmentPlane struct {
	*stagedControlClient
	beginErr error
	begun    int
	finishes []string
	claims   int
}

func (p *segmentPlane) BeginSegment(_ context.Context, _ boardclient.LeaseToken,
	attemptID, _ string, _, _ time.Duration) (store.BoardSegment, error) {
	p.begun++
	if p.beginErr != nil {
		return store.BoardSegment{}, p.beginErr
	}
	return store.BoardSegment{ID: attemptID}, nil
}

func (p *segmentPlane) FinishSegment(_ context.Context, _ boardclient.LeaseToken, _, _, outcome string) error {
	p.finishes = append(p.finishes, outcome)
	return nil
}

func (p *segmentPlane) ClaimNextHILAttempt(_ context.Context, _, _, _ string, _ int, _ int64,
	_ float64, _ json.RawMessage) (*store.BoardHILAssignment, error) {
	p.claims++
	return nil, nil
}

// holdTheGate occupies the local segment gate the way an in-flight
// reconciliation does, so the only way through enterSegment is the context.
func holdTheGate(t *testing.T, agent *Agent) {
	t.Helper()
	select {
	case agent.segmentGate <- struct{}{}:
	default:
		t.Fatal("the segment gate was already held")
	}
}

func TestRunSegmentRefusesWhileTheLocalGateIsHeld(t *testing.T) {
	// The gate serializes reconciliation against hardware work in this
	// process. A caller that cannot take it waits on its own context and
	// leaves with the context's refusal, never with a segment.
	_, active, _ := stagedBoard(t)
	plane := &segmentPlane{stagedControlClient: &stagedControlClient{status: active}}
	agent := stagedAgent(t, plane, fixedHighWater{value: active.Generation}, time.Now())
	holdTheGate(t, agent)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	ran := false
	segment, err := agent.RunSegment(ctx, tokenFor(active), active.Lease.WaiterID, "flash",
		time.Second, 0, func(context.Context) error { ran = true; return nil })
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("error = %v", err)
	}
	if ran || plane.begun != 0 || segment.ID != "" {
		t.Fatalf("a held gate still began work: ran=%v begun=%d segment=%q", ran, plane.begun, segment.ID)
	}
}

func TestClaimNextHILAttemptRefusesWhileTheLocalGateIsHeld(t *testing.T) {
	_, active, _ := stagedBoard(t)
	plane := &segmentPlane{stagedControlClient: &stagedControlClient{status: active}}
	agent := stagedAgent(t, plane, fixedHighWater{value: active.Generation}, time.Now())
	holdTheGate(t, agent)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	assignment, err := agent.ClaimNextHILAttempt(ctx, tokenFor(active), "bench-01", 2, 1<<30, 0.5, nil)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("error = %v", err)
	}
	if assignment != nil || plane.claims != 0 {
		t.Fatalf("a held gate still claimed an attempt: claims=%d", plane.claims)
	}
}

func TestRunSegmentCarriesTheRefusalToBeginADurableSegment(t *testing.T) {
	// Every local authorization held, so the refusal came from the server's
	// own transaction. It reaches the caller as it arrived, and nothing is
	// finished, because nothing was ever begun.
	_, active, _ := stagedBoard(t)
	refused := errors.New("board segment transaction rolled back")
	plane := &segmentPlane{stagedControlClient: &stagedControlClient{status: active}, beginErr: refused}
	agent := stagedAgent(t, plane, fixedHighWater{value: active.Generation}, time.Now())
	ran := false
	segment, err := agent.RunSegment(context.Background(), tokenFor(active), active.Lease.WaiterID, "flash",
		time.Second, 0, func(context.Context) error { ran = true; return nil })
	if !errors.Is(err, refused) {
		t.Fatalf("error = %v", err)
	}
	if plane.begun != 1 {
		t.Fatalf("begin was attempted %d times", plane.begun)
	}
	if ran || segment.ID != "" || len(plane.finishes) != 0 {
		t.Fatalf("a refused begin still moved on: ran=%v segment=%q finishes=%v", ran, segment.ID, plane.finishes)
	}
}

// tokenFor is the lease token an acknowledged snapshot authorizes.
func tokenFor(snapshot board.Snapshot) boardclient.LeaseToken {
	return boardclient.LeaseToken{BoardID: snapshot.BoardID, RequestID: snapshot.Lease.WaiterID,
		LeaseID: snapshot.Lease.ID, Generation: snapshot.Generation,
		ExpiresAt: snapshot.Lease.ExpiresAt, Version: snapshot.Version}
}
