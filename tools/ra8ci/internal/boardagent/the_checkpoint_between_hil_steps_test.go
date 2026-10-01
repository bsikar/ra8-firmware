// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// A step that outlives the attempt it belongs to is recorded as timed out,
// not merely failed, and the attempt itself is marked as having hit its
// deadline. That distinction is what tells an operator the board was still
// working when time ran out rather than reporting a fault.
func TestAStepThatOutlivesItsAttemptIsRecordedAsTimedOut(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	started := time.Now().UTC()
	assignment.Attempt.StartedAt = started
	assignment.Attempt.DeadlineAt = started.Add(150 * time.Millisecond)
	assignment.HILTiming.Decision.ValidityWindow = 50 * time.Millisecond
	assignment.HILTiming.Decision.FlashRestoreBound = 10 * time.Millisecond
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(stepCtx context.Context, _ string, _ catalog.Task, _ catalog.Step) (int, error) {
			<-stepCtx.Done()
			return -1, stepCtx.Err()
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if completion.Result != "timed_out" || !completion.HitDeadline {
		t.Fatalf("an attempt that ran out of time did not say so: %+v", completion)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "timed_out" {
		t.Fatalf("steps = %+v", completion.Steps)
	}
	if completion.Steps[0].DurationNS <= 0 {
		t.Fatalf("the step recorded no measured duration: %+v", completion.Steps[0])
	}
}

// Between steps the agent reconciles, which is where a lease that moved under
// it is caught. A deadline the server pushed to a version the local fence
// cannot follow stops the attempt at that checkpoint: the step already run is
// kept, and the next one never starts.
func TestALeaseThatMovedBetweenStepsStopsTheAttempt(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	if len(assignment.Task.Steps) < 2 {
		t.Fatalf("fixture no longer has a checkpoint between steps: %d", len(assignment.Task.Steps))
	}
	runs := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(_ context.Context, _ string, _ catalog.Task, _ catalog.Step) (int, error) {
			runs++
			moved := *client.state.Lease
			moved.DeadlineVersion++
			moved.ExpiresAt = time.Now().UTC().Add(-time.Hour)
			client.state.Lease = &moved
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if runs != 1 {
		t.Fatalf("the attempt continued past the checkpoint: runs = %d", runs)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "succeeded" {
		t.Fatalf("the step that did run was not kept: %+v", completion.Steps)
	}
	if completion.Result != "failed" || completion.EvidenceComplete {
		t.Fatalf("an interrupted attempt was not failed: %+v", completion)
	}
	if completion.Reason == "" || completion.Reason == "HIL attempt failed" {
		t.Fatalf("the reason does not carry the refusal from the checkpoint: %q", completion.Reason)
	}
}
