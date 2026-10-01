// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// An attempt refused before it reaches the board still has to record a step,
// and the step is stamped from the local clock. Where that clock reads
// earlier than the attempt's own start, the record would carry a step that
// began before the attempt it belongs to: a row no reader of the history can
// make sense of, and one that sorts ahead of the attempt that produced it.
// The start is therefore the floor, and the recorded step is pinned to it
// rather than to a clock that disagrees.
//
// A future start is the honest way to reach that, since the server pins
// StartedAt and the agent cannot assume the two clocks agree. Nothing here
// is skewed: the fixture simply states an attempt that starts later.
func TestAStepIsNeverRecordedBeforeTheAttemptItBelongsTo(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	if err := catalog.ValidateTask(assignment.Task); err != nil {
		t.Fatalf("test task invalid: %v", err)
	}
	// The attempt starts an hour from now and the refusal happens at once,
	// so the clock the step would otherwise be stamped from is an hour
	// behind the attempt's own start.
	started := time.Now().UTC().Add(time.Hour)
	assignment.Attempt.StartedAt = started
	assignment.Attempt.DeadlineAt = started.Add(time.Minute)
	// Unpinned timing is refused before any step runs, which is the
	// cheapest refusal that still reaches the recording.
	assignment.HILTiming = nil

	reached := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			reached++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if reached != 0 {
		t.Fatalf("the refusal still reached the board %d times", reached)
	}
	if completion.Result != "failed" {
		t.Fatalf("an unpinned attempt did not fail: %+v", completion)
	}
	if len(completion.Steps) != 1 {
		t.Fatalf("a refused attempt recorded %d steps, want the one it was refused on", len(completion.Steps))
	}

	step := completion.Steps[0]
	if step.Key != assignment.Task.Steps[0].Name {
		t.Fatalf("the refusal was recorded against %q, not the step it stopped at", step.Key)
	}
	if step.StartedAt.Before(assignment.Attempt.StartedAt) {
		t.Fatalf("the step starts %v before the attempt it belongs to", assignment.Attempt.StartedAt.Sub(step.StartedAt))
	}
	if !step.StartedAt.Equal(assignment.Attempt.StartedAt) {
		t.Fatalf("the step was stamped %v rather than at the attempt's own start", step.StartedAt)
	}
	if !step.EndedAt.After(step.StartedAt) || step.DurationNS <= 0 {
		t.Fatalf("the recorded step does not end after it starts: %+v", step)
	}
}

// The floor is a floor, not a replacement: an attempt already under way is
// recorded from the clock, so the refusal keeps the time it actually
// happened rather than being backdated to the attempt's start.
func TestAnAttemptAlreadyUnderWayIsRecordedFromTheClock(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	started := time.Now().UTC().Add(-time.Hour)
	assignment.Attempt.StartedAt = started
	assignment.Attempt.DeadlineAt = time.Now().UTC().Add(time.Minute)
	assignment.HILTiming = nil

	before := time.Now().UTC()
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if len(completion.Steps) != 1 {
		t.Fatalf("a refused attempt recorded %d steps", len(completion.Steps))
	}
	if step := completion.Steps[0]; step.StartedAt.Before(before) {
		t.Fatalf("the refusal was backdated to %v, an hour before it happened", step.StartedAt)
	}
}
