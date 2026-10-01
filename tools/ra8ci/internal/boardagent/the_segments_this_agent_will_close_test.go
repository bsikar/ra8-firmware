// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// hookedSegmentClient runs one action inside BeginSegment, which is the only
// moment where the segment is durable on the server but the operation has not
// started. Every refusal below that point has to be reported together with a
// finished segment, never left open.
type hookedSegmentClient struct {
	*testSegmentControlClient
	onBegin func()
}

func (c *hookedSegmentClient) BeginSegment(ctx context.Context, token boardclient.LeaseToken,
	attemptID, key string, bound, margin time.Duration) (store.BoardSegment, error) {
	if c.onBegin != nil {
		c.onBegin()
	}
	return c.testSegmentControlClient.BeginSegment(ctx, token, attemptID, key, bound, margin)
}

// hookedAgent rebuilds the fixture with an action bound into BeginSegment.
func hookedAgent(t *testing.T, onBegin func(*Agent, boardclient.LeaseToken)) (*Agent, *testSegmentControlClient, boardclient.LeaseToken) {
	t.Helper()
	agent, client, token := newActiveSegmentAgent(t)
	agent.client = &hookedSegmentClient{testSegmentControlClient: client,
		onBegin: func() { onBegin(agent, token) }}
	return agent, client, token
}

// A segment is an indivisible hardware operation, so its bounds are judged
// before the server is told anything. A bound of zero or one above the hour
// ceiling is a misconfigured caller, not a short segment.
func TestRunSegmentRefusesBoundsItCannotHold(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	const attempt = "01996f90-3415-7cfe-8ff1-600058131aff"

	otherBoard := token
	otherBoard.BoardID = "ek-ra8m1"

	ran := false
	operation := func(context.Context) error { ran = true; return nil }

	for name, item := range map[string]struct {
		token  boardclient.LeaseToken
		bound  time.Duration
		margin time.Duration
		op     func(context.Context) error
	}{
		"no operation":                 {token, time.Second, 0, nil},
		"another board's token":        {otherBoard, time.Second, 0, operation},
		"no bound":                     {token, 0, 0, operation},
		"a bound that runs backwards":  {token, -time.Second, 0, operation},
		"a bound above the ceiling":    {token, maxBoardOperation + time.Nanosecond, 0, operation},
		"a margin that runs backwards": {token, time.Second, -time.Nanosecond, operation},
		"a margin above the ceiling":   {token, time.Second, maxBoardOperation + time.Nanosecond, operation},
	} {
		_, err := agent.RunSegment(context.Background(), item.token, attempt, "bounds", item.bound, item.margin, item.op)
		if !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
	}

	var noContext context.Context
	if _, err := agent.RunSegment(noContext, token, attempt, "bounds", time.Second, 0, operation); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no context: err = %v, want ErrInvalidAgent", err)
	}
	var absent *Agent
	if _, err := absent.RunSegment(context.Background(), token, attempt, "bounds", time.Second, 0, operation); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no agent: err = %v, want ErrInvalidAgent", err)
	}
	if ran || client.begins != 0 || len(client.finishes) != 0 {
		t.Fatalf("a refused segment still reached the board: ran=%v begins=%d finishes=%v",
			ran, client.begins, client.finishes)
	}

	// The ceiling itself is not an invalid argument. A bound exactly at it is
	// judged by the lease instead, which is what tells an operator the
	// configuration was fine and the board simply was not held long enough.
	if _, err := agent.RunSegment(context.Background(), token, attempt, "at the ceiling",
		maxBoardOperation, maxBoardOperation, operation); errors.Is(err, ErrInvalidAgent) || err == nil {
		t.Fatalf("a bound at the ceiling: err = %v, want the lease's own refusal", err)
	}
	if ran || client.begins != 0 {
		t.Fatalf("the ceiling segment reached the board: ran=%v begins=%d", ran, client.begins)
	}

	if _, err := agent.RunSegment(context.Background(), token, attempt, "sound",
		time.Second, 0, operation); err != nil {
		t.Fatalf("a sound segment was refused: %v", err)
	}
	if !ran || client.begins != 1 || len(client.finishes) != 1 || client.finishes[0] != "completed" {
		t.Fatalf("the sound segment did not complete: ran=%v begins=%d finishes=%v",
			ran, client.begins, client.finishes)
	}
}

// A client with no durable segment path is named rather than letting hardware
// run with nothing recording that it did.
func TestRunSegmentRefusesAClientThatCannotRecordIt(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	agent.client = agent.client.(*testSegmentControlClient).testControlClient
	ran := false
	_, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
		"unrecorded", time.Second, 0, func(context.Context) error { ran = true; return nil })
	if !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("err = %v, want ErrInvalidAgent", err)
	}
	if ran {
		t.Fatal("the operation ran without a durable segment")
	}
}

// The bound covers the server round trip too. When opening the segment has
// already spent it, the operation must not start, and the segment it just
// opened must be finished rather than left running on the board.
func TestRunSegmentFinishesASegmentWhoseBoundWasSpentOpeningIt(t *testing.T) {
	const bound = 2 * time.Second
	var offset time.Duration
	agent, client, token := hookedAgent(t, func(*Agent, boardclient.LeaseToken) { offset += bound })
	agent.clock = func() time.Time { return time.Now().Add(offset) }

	ran := false
	segment, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
		"spent", bound, 0, func(context.Context) error { ran = true; return nil })
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("err = %v, want context.DeadlineExceeded", err)
	}
	if ran {
		t.Fatal("the operation started with no bound left")
	}
	if segment.ID == "" {
		t.Fatal("the open segment was not handed back to the caller")
	}
	if len(client.finishes) != 1 || client.finishes[0] != "failed" {
		t.Fatalf("finishes = %v, want one failed", client.finishes)
	}
}

// The durable generation is read again after the segment opens. If it moved,
// this agent no longer holds the board it was authorized for, and the segment
// is closed rather than used.
func TestRunSegmentFinishesASegmentWhoseDurableGenerationMoved(t *testing.T) {
	agent, client, token := hookedAgent(t, func(a *Agent, tok boardclient.LeaseToken) {
		if err := a.highWater.Advance(tok.Generation + 1); err != nil {
			t.Error(err)
		}
	})
	ran := false
	_, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
		"moved", time.Second, 0, func(context.Context) error { ran = true; return nil })
	if !errors.Is(err, boardclient.ErrStaleLease) {
		t.Fatalf("err = %v, want ErrStaleLease", err)
	}
	if ran {
		t.Fatal("the operation ran under a generation this agent no longer holds")
	}
	if len(client.finishes) != 1 || client.finishes[0] != "failed" {
		t.Fatalf("finishes = %v, want one failed", client.finishes)
	}
}

// The local deadline fence is the last authorization, and it is re-read after
// the segment opens. An absent fence is recovery, not a segment to run blind.
func TestRunSegmentFinishesASegmentWithNoLocalFence(t *testing.T) {
	agent, client, token := hookedAgent(t, func(a *Agent, _ boardclient.LeaseToken) { a.clearDeadlineFence() })
	ran := false
	_, err := agent.RunSegment(context.Background(), token, "01996f90-3415-7cfe-8ff1-600058131aff",
		"unfenced", time.Second, 0, func(context.Context) error { ran = true; return nil })
	var refusal *board.Error
	if !errors.As(err, &refusal) || refusal.Code != board.RecoveryNecessary {
		t.Fatalf("err = %v, want RecoveryNecessary", err)
	}
	if ran {
		t.Fatal("the operation ran with no local deadline fence")
	}
	if len(client.finishes) != 1 || client.finishes[0] != "failed" {
		t.Fatalf("finishes = %v, want one failed", client.finishes)
	}
}

// A caller that cancels its own context is yielding the board cooperatively,
// which the server must be able to tell apart from an operation that failed on
// its own terms.
func TestRunSegmentReportsACooperativeYieldApartFromAFailure(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	_, err := agent.RunSegment(ctx, token, "01996f90-3415-7cfe-8ff1-600058131aff", "yielding",
		time.Second, 0, func(operationCtx context.Context) error {
			cancel()
			<-operationCtx.Done()
			return nil
		})
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("err = %v, want context.Canceled", err)
	}
	if len(client.finishes) != 1 || client.finishes[0] != "yielded" {
		t.Fatalf("finishes = %v, want one yielded", client.finishes)
	}

	failing, _, failingToken := newActiveSegmentAgent(t)
	refused := errors.New("the fixture refused")
	_, err = failing.RunSegment(context.Background(), failingToken, "01996f90-3415-7cfe-8ff1-600058131aff",
		"failing", time.Second, 0, func(context.Context) error { return refused })
	if !errors.Is(err, refused) {
		t.Fatalf("err = %v, want the operation's own error", err)
	}
	if len(client.finishes) != 1 {
		t.Fatalf("the yielding client saw another finish: %v", client.finishes)
	}
}
