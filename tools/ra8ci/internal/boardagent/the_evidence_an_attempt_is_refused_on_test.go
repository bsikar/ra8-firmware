// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// A refusal before any step runs still has to leave a record: the attempt was
// claimed on the server, so it cannot simply be abandoned unexplained.
func TestAnAttemptWithNoPinnedTimingIsRefusedBeforeTheBoard(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	assignment.HILTiming = nil
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("an unpinned attempt reached the board: steps=%d begins=%d", steps, client.begins)
	}
	if completion.Result != "failed" || completion.EvidenceComplete {
		t.Fatalf("an unpinned attempt did not fail: %+v", completion)
	}
	if !strings.Contains(completion.Reason, "HIL timing evidence") {
		t.Fatalf("the reason does not name the missing evidence: %q", completion.Reason)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "failed" ||
		completion.Steps[0].Key != assignment.Task.Steps[0].Name {
		t.Fatalf("the refusal left no step record: %+v", completion.Steps)
	}
}

// The observation window the server pinned is judged against the attempt row
// it will be written into, not against the task's own deadline: a window that
// outlives the attempt cannot be observed to its end.
func TestAnObservationBudgetBeyondTheAttemptIsRefused(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	started := assignment.Attempt.StartedAt
	assignment.Attempt.DeadlineAt = started.Add(5 * time.Second)
	if assignment.HILTiming.Decision.ValidityWindow <= 5*time.Second {
		t.Fatalf("fixture no longer pins a window past the attempt: %v",
			assignment.HILTiming.Decision.ValidityWindow)
	}
	steps := 0
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
			steps++
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if steps != 0 || client.begins != 0 {
		t.Fatalf("an unobservable attempt reached the board: steps=%d begins=%d", steps, client.begins)
	}
	if !strings.Contains(completion.Reason, "observation budget") {
		t.Fatalf("the reason does not name the budget: %q", completion.Reason)
	}
}

// The state a refused step is recorded in is how an operator later tells a
// board that failed from an attempt that simply ran out of time or was taken
// away, so each of the three is pinned separately.
func TestTheStateARefusedStepIsRecordedIn(t *testing.T) {
	t.Run("an attempt whose deadline already passed reads as timed out", func(t *testing.T) {
		agent, _, token := newActiveSegmentAgent(t)
		assignment := hilAttemptAssignment(token.BoardID)
		assignment.HILTiming = nil
		assignment.Attempt.StartedAt = time.Now().UTC().Add(-2 * time.Minute)
		assignment.Attempt.DeadlineAt = assignment.Attempt.StartedAt.Add(time.Minute)
		completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
			20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
				return 0, nil
			})
		if err != nil {
			t.Fatalf("terminal evidence was not persisted: %v", err)
		}
		if len(completion.Steps) != 1 || completion.Steps[0].State != "timed_out" {
			t.Fatalf("steps = %+v", completion.Steps)
		}
		if !completion.Steps[0].EndedAt.After(completion.Steps[0].StartedAt) {
			t.Fatalf("the record does not move forward in time: %+v", completion.Steps[0])
		}
	})

	t.Run("an attempt taken away reads as cancelled", func(t *testing.T) {
		agent, _, token := newActiveSegmentAgent(t)
		assignment := hilAttemptAssignment(token.BoardID)
		assignment.HILTiming = nil
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		completion, err := agent.RunHILAttempt(ctx, token, hilAttemptRoot(t), assignment,
			20*time.Second, 0, func(context.Context, string, catalog.Task, catalog.Step) (int, error) {
				return 0, nil
			})
		if err != nil {
			t.Fatalf("terminal evidence was not persisted even though the caller left: %v", err)
		}
		if len(completion.Steps) != 1 || completion.Steps[0].State != "cancelled" {
			t.Fatalf("steps = %+v", completion.Steps)
		}
		if completion.Result != "cancelled" {
			t.Fatalf("result = %q", completion.Result)
		}
	})
}
