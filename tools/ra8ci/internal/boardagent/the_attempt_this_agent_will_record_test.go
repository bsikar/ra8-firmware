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
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// An attempt reaches real hardware, so everything it is held to is judged
// before the first step runs. Each case below breaks exactly one of those
// conditions on an otherwise sound assignment, and none of them may reach the
// runner or open a segment.
func TestRunHILAttemptRefusesAnAssignmentItCannotStand(t *testing.T) {
	sound := func(t *testing.T) (*Agent, *testSegmentControlClient, boardclient.LeaseToken, store.BoardHILAssignment) {
		t.Helper()
		agent, client, token := newActiveSegmentAgent(t)
		return agent, client, token, hilAttemptAssignment(token.BoardID)
	}

	for name, item := range map[string]struct {
		token      func(boardclient.LeaseToken) boardclient.LeaseToken
		assignment func(*store.BoardHILAssignment)
		safety     time.Duration
		margin     time.Duration
		noRunner   bool
	}{
		"no runner": {noRunner: true, safety: 20 * time.Second},
		"another board's token": {safety: 20 * time.Second,
			token: func(tok boardclient.LeaseToken) boardclient.LeaseToken { tok.BoardID = "ek-ra8m1"; return tok }},
		"a lease that is not an id": {safety: 20 * time.Second,
			token: func(tok boardclient.LeaseToken) boardclient.LeaseToken { tok.LeaseID = "lease-1"; return tok }},
		"no generation": {safety: 20 * time.Second,
			token: func(tok boardclient.LeaseToken) boardclient.LeaseToken { tok.Generation = 0; return tok }},
		"an attempt that is not running": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Attempt.State = "queued" }},
		"an attempt id that is not an id": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Attempt.ID = "attempt-1" }},
		"an attempt that never started": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Attempt.StartedAt = time.Time{} }},
		"an attempt with no deadline": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Attempt.DeadlineAt = time.Time{} }},
		"a deadline that precedes the start": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Attempt.DeadlineAt = a.Attempt.StartedAt }},
		"a task that is not HIL": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Task.Scope = "unit" }},
		"a task with no HIL block": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Task.HIL = nil }},
		"another board's task": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Task.HIL.BoardID = "ek-ra8m1" }},
		"a task this runner cannot host": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Task.OS = []string{"darwin"} }},
		"a task the catalog refuses": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.Task.Version = 0 }},
		"a catalog that is not pinned": {safety: 20 * time.Second,
			assignment: func(a *store.BoardHILAssignment) { a.CatalogSHA256 = "" }},
		"a safety maximum that runs backwards": {safety: -time.Nanosecond},
		"a safety maximum above the hour":      {safety: time.Hour + time.Nanosecond},
		"a margin that runs backwards":         {safety: 20 * time.Second, margin: -time.Nanosecond},
		"a margin above the ceiling":           {safety: 20 * time.Second, margin: maxBoardOperation + time.Nanosecond},
	} {
		agent, client, token, assignment := sound(t)
		if item.token != nil {
			token = item.token(token)
		}
		if item.assignment != nil {
			item.assignment(&assignment)
		}
		steps := 0
		var runner HILStepRunner
		if !item.noRunner {
			runner = func(context.Context, string, catalog.Task, catalog.Step) (int, error) { steps++; return 0, nil }
		}
		_, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
			item.safety, item.margin, runner)
		if !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
		if steps != 0 || client.begins != 0 || client.completed != 0 {
			t.Errorf("%s: a refused attempt reached the board: steps=%d begins=%d completed=%d",
				name, steps, client.begins, client.completed)
		}
	}

	var noContext context.Context
	agent, _, token, assignment := sound(t)
	if _, err := agent.RunHILAttempt(noContext, token, hilAttemptRoot(t), assignment, 20*time.Second, 0,
		func(context.Context, string, catalog.Task, catalog.Step) (int, error) { return 0, nil }); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no context: err = %v, want ErrInvalidAgent", err)
	}
	var absent *Agent
	if _, err := absent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment, 20*time.Second, 0,
		func(context.Context, string, catalog.Task, catalog.Step) (int, error) { return 0, nil }); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no agent: err = %v, want ErrInvalidAgent", err)
	}
}

// A step that exits non-zero is evidence, not a crash: the exit code is carried
// into the record so a reader knows what the board actually did, and the
// remaining steps do not run on a fixture that already failed.
func TestRunHILAttemptCarriesAFailedStepsExitCodeIntoTheRecord(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	seen := make([]string, 0, 2)
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, time.Second, func(_ context.Context, _ string, _ catalog.Task, step catalog.Step) (int, error) {
			seen = append(seen, step.Name)
			return 3, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if len(seen) != 1 || seen[0] != "flash" {
		t.Fatalf("steps ran past a failed fixture: %v", seen)
	}
	if completion.Result != "failed" || completion.EvidenceComplete {
		t.Fatalf("completion = %+v, want a failed attempt with incomplete evidence", completion)
	}
	if completion.ChildExitCode == nil || *completion.ChildExitCode != 3 {
		t.Fatalf("child exit code = %v, want 3", completion.ChildExitCode)
	}
	if !strings.Contains(completion.Reason, "status 3") {
		t.Fatalf("reason does not name the exit status: %q", completion.Reason)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "failed" ||
		completion.Steps[0].ChildExitCode == nil || *completion.Steps[0].ChildExitCode != 3 {
		t.Fatalf("recorded steps = %+v", completion.Steps)
	}
	if len(client.finishes) != 1 || client.finishes[0] != "failed" ||
		client.completed != 1 || client.completion.Result != "failed" {
		t.Fatalf("the server saw finishes=%v completed=%d result=%q",
			client.finishes, client.completed, client.completion.Result)
	}
}

// A yield asked for between steps is a cooperative handover, not a failure of
// the fixture. The attempt stops at the checkpoint and the record says
// preempted, so a scheduler can retry it rather than treating the board as
// broken.
func TestRunHILAttemptStopsAtACheckpointWhenTheBoardIsAskedToYield(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	seen := make([]string, 0, 2)
	completion, err := agent.RunHILAttempt(context.Background(), token, hilAttemptRoot(t), assignment,
		20*time.Second, time.Second, func(_ context.Context, _ string, _ catalog.Task, step catalog.Step) (int, error) {
			seen = append(seen, step.Name)
			client.state.Phase = board.YieldRequested
			return 0, nil
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted: %v", err)
	}
	if len(seen) != 1 || seen[0] != "flash" {
		t.Fatalf("the attempt ran past the checkpoint: %v", seen)
	}
	if completion.Result != "preempted" {
		t.Fatalf("completion = %+v, want preempted", completion)
	}
	if completion.EvidenceComplete || completion.HitDeadline {
		t.Fatalf("a preempted attempt claimed complete evidence or a deadline: %+v", completion)
	}
	if !strings.Contains(completion.Reason, "yielded") {
		t.Fatalf("reason does not name the yield: %q", completion.Reason)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "succeeded" {
		t.Fatalf("the step that did run was not recorded as it happened: %+v", completion.Steps)
	}
	if client.completed != 1 || client.completion.Result != "preempted" {
		t.Fatalf("the server saw completed=%d result=%q", client.completed, client.completion.Result)
	}
}

// A caller that cancels mid-step is recorded as cancelled rather than as a
// board that failed, and the terminal record is still persisted because the
// server has an attempt open either way.
func TestRunHILAttemptPersistsACancelledAttempt(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment := hilAttemptAssignment(token.BoardID)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	completion, err := agent.RunHILAttempt(ctx, token, hilAttemptRoot(t), assignment,
		20*time.Second, time.Second, func(stepCtx context.Context, _ string, _ catalog.Task, _ catalog.Step) (int, error) {
			cancel()
			<-stepCtx.Done()
			return -1, stepCtx.Err()
		})
	if err != nil {
		t.Fatalf("terminal evidence was not persisted after cancellation: %v", err)
	}
	if completion.Result != "cancelled" || completion.EvidenceComplete {
		t.Fatalf("completion = %+v, want a cancelled attempt", completion)
	}
	if completion.ChildExitCode != nil {
		t.Fatalf("a cancelled step reported a child exit code: %v", *completion.ChildExitCode)
	}
	if len(completion.Steps) != 1 || completion.Steps[0].State != "cancelled" {
		t.Fatalf("recorded steps = %+v", completion.Steps)
	}
	if client.completed != 1 || client.completion.Result != "cancelled" {
		t.Fatalf("the server saw completed=%d result=%q", client.completed, client.completion.Result)
	}
}
