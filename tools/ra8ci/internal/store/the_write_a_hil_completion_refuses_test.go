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

// The write a HIL completion refuses to make.
//
// A completion is checked long before anything is written down, but the
// last few refusals happen with the pen already in hand: a success that
// carries a failed step, evidence that is already on the record, and a
// task that moved out of running while the attempt was in flight. Each
// one has to leave the attempt exactly as it found it.
func TestIntegrationTheWriteAHILCompletionRefuses(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "hil-refused-write-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("e", 64)
	trustedCommit := strings.Repeat("f", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "a refused completion", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-refused-write", Version: 1, Tier: "required", Scope: "hil",
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
		SnapshotSHA256: strings.Repeat("a", 64), CatalogSHA256: digest,
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

	// Both steps get a real opened and closed durable segment, so every
	// refusal below is the one under test rather than missing evidence.
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
		carried := append([]HILStep(nil), steps...)
		return BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
			Generation: active.Generation, Result: "succeeded", ChildExitCode: &zero,
			EvidenceComplete: true, Steps: carried}
	}

	t.Run("a success carrying a step that failed", func(t *testing.T) {
		completion := sound()
		completion.Steps[1].State = "failed"
		completion.Steps[1].ChildExitCode = nil
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "successful HIL result has unsuccessful step") {
			t.Fatalf("a success was recorded over a failed step: %v", err)
		}
	})

	t.Run("evidence that is already on the record", func(t *testing.T) {
		// The step table is keyed by attempt and step, so a key already
		// written is a second report of the same work, not an update.
		if _, err := pool.Exec(ctx, `INSERT INTO task_steps
			(attempt_id,step_key,ordinal,phase,started_at,ended_at,duration_ns,state,child_exit_code)
			VALUES ($1,'flash',0,'execute',$2,$3,1,'succeeded',0)`,
			attempt.ID, steps[0].StartedAt, steps[0].EndedAt); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "DELETE FROM task_steps WHERE attempt_id=$1", attempt.ID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "persist HIL step flash") {
			t.Fatalf("a second report of the same step was written: %v", err)
		}
	})

	t.Run("a task that moved out of running under the attempt", func(t *testing.T) {
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='failed',ended_at=clock_timestamp()
			WHERE id=$1`, run.Tasks[0].ID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, `UPDATE tasks SET state='running',ended_at=NULL
				WHERE id=$1`, run.Tasks[0].ID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "HIL task is no longer running") {
			t.Fatalf("a completion finished an attempt whose task had moved: %v", err)
		}
	})

	t.Run("the attempt is exactly as the refusals found it", func(t *testing.T) {
		var state string
		var written int
		if err := pool.QueryRow(ctx, `SELECT a.state,
			(SELECT count(*) FROM task_steps WHERE attempt_id=a.id)
			FROM task_attempts a WHERE a.id=$1`, attempt.ID).Scan(&state, &written); err != nil {
			t.Fatal(err)
		}
		if state != "running" || written != 0 {
			t.Fatalf("the refusals left the attempt %s with %d steps written", state, written)
		}
	})

	t.Run("the completion the attempt was waiting for", func(t *testing.T) {
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound(), definitions, trustedCommit); err != nil {
			t.Fatalf("the sound completion was refused: %v", err)
		}
		var attemptState, taskState string
		var written int
		if err := pool.QueryRow(ctx, `SELECT a.state,t.state,
			(SELECT count(*) FROM task_steps WHERE attempt_id=a.id)
			FROM task_attempts a JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, attempt.ID).
			Scan(&attemptState, &taskState, &written); err != nil {
			t.Fatal(err)
		}
		if attemptState != "succeeded" || taskState != "succeeded" || written != len(task.Steps) {
			t.Fatalf("the completion landed as attempt=%s task=%s steps=%d",
				attemptState, taskState, written)
		}
	})
}
