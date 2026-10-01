//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The replay a finished HIL completion allows.
//
// A board agent that loses its answer repeats the completion, and the
// plane has to accept the repeat without writing anything twice. What
// it must not accept is a repeat that tells a different story: another
// terminal result, another task state, or step evidence that has moved
// since it was written down.
func TestIntegrationTheReplayAFinishedHILCompletionAllows(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "hil-replay-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("1", 64)
	trustedCommit := strings.Repeat("2", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "a replayed completion", Duration: 2 * time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("board lease did not activate: phase=%s err=%v", active.Phase, err)
	}

	hil := &catalog.HILTask{BoardID: boardID, BoardModel: "EK-RA8D2",
		ManifestPath:  "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily: "uart-demo", Mode: "uart_scrape", ObservationStep: "observe",
		FlashRestoreSeconds: 10}
	task := catalog.Task{Name: "hil-replayed", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive", HIL: hil,
		Steps: []catalog.Step{{Name: "flash", Program: "test-adapter"}, {Name: "observe", Program: "test-adapter"}},
		Retry: catalog.RetryPolicy{MaxAttempts: 1}}
	if err := catalog.ValidateTask(task); err != nil {
		t.Fatalf("invalid test HIL task: %v", err)
	}
	encodedHIL, err := json.Marshal(hil)
	if err != nil {
		t.Fatal(err)
	}
	arguments := append([]byte(`{"argv":[],"hil":`), encodedHIL...)
	arguments = append(arguments, byte(125))
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: trustedCommit,
		SnapshotSHA256: strings.Repeat("3", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	definitions := completionTestCatalog{digest: digest, task: task}
	assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
		testStart(run.Tasks[0].ID), definitions, trustedCommit)
	if err != nil || assignment == nil {
		t.Fatalf("claim HIL assignment: %v", err)
	}
	attempt := assignment.Attempt

	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}
	zero := 0
	steps := make([]HILStep, 0, len(task.Steps))
	for _, definition := range task.Steps {
		segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID,
			definition.Name, 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin %s segment: %v", definition.Name, err)
		}
		steps = append(steps, HILStep{Key: definition.Name, StartedAt: segment.StartedAt,
			EndedAt: segment.StartedAt.Add(5 * time.Millisecond), DurationNS: (5 * time.Millisecond).Nanoseconds(),
			State: "succeeded", ChildExitCode: &zero})
		time.Sleep(10 * time.Millisecond)
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attempt.ID, "completed"); err != nil {
			t.Fatalf("finish %s segment: %v", definition.Name, err)
		}
	}
	sound := func() BoardHILCompletion {
		return BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
			Generation: active.Generation, Result: "succeeded", ChildExitCode: &zero,
			EvidenceComplete: true, Steps: append([]HILStep(nil), steps...)}
	}
	if err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit); err != nil {
		t.Fatalf("complete HIL attempt: %v", err)
	}

	t.Run("a repeat carrying another terminal result", func(t *testing.T) {
		completion := sound()
		completion.Result = "failed"
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "retry changed terminal result") {
			t.Fatalf("a repeat rewrote the terminal result: %v", err)
		}
	})

	t.Run("a repeat carrying another reason", func(t *testing.T) {
		completion := sound()
		completion.Reason = "a reason the first completion never carried"
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "retry changed terminal result") {
			t.Fatalf("a repeat added a reason after the fact: %v", err)
		}
	})

	t.Run("a repeat carrying step evidence that has moved", func(t *testing.T) {
		completion := sound()
		completion.Steps[1].DurationNS = steps[1].DurationNS + 1
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "retry changed step evidence") {
			t.Fatalf("a repeat moved the step evidence: %v", err)
		}
	})

	t.Run("a repeat over a task that has since moved", func(t *testing.T) {
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='failed' WHERE id=$1`,
			run.Tasks[0].ID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, `UPDATE tasks SET state='succeeded' WHERE id=$1`,
				run.Tasks[0].ID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "retry changed task state") {
			t.Fatalf("a repeat accepted a task state it did not write: %v", err)
		}
	})

	t.Run("the repeat the board agent is entitled to", func(t *testing.T) {
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit); err != nil {
			t.Fatalf("the honest repeat was refused: %v", err)
		}
		var attemptState, taskState string
		var written, finished int
		if err := pool.QueryRow(ctx, `SELECT a.state,t.state,
			(SELECT count(*) FROM task_steps WHERE attempt_id=a.id),
			(SELECT count(*) FROM audit WHERE action='task.attempt.finished' AND target_id=t.id::text)
			FROM task_attempts a JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, attempt.ID).
			Scan(&attemptState, &taskState, &written, &finished); err != nil {
			t.Fatal(err)
		}
		if attemptState != "succeeded" || taskState != "succeeded" ||
			written != len(task.Steps) || finished != 1 {
			t.Fatalf("the repeat wrote again: attempt=%s task=%s steps=%d finished audits=%d",
				attemptState, taskState, written, finished)
		}
	})
}
