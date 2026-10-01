//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// Preemption is the one HIL completion that does not end the work.
//
// When a human takes the board back, the agent's attempt is over but the
// task is not: it goes back to the queue to be run again later, which is
// the single backwards edge the task machine carries on purpose. The
// exception is a run that was already cancelled, where there is nothing to
// come back to, so the same report has to land as a cancellation instead.
//
// Both cases are terminal for their attempt, so each gets its own run off
// the same still-active board lease. The cancelled case runs first on
// purpose: the requeue case leaves a scheduled task behind, and the next
// claim on this lease would pick it up.
func TestIntegrationTheQueueAPreemptedHILAttemptReturnsTo(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "hil-preempt-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "HIL preemption proof", Duration: 3 * time.Minute}
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
	task := catalog.Task{Name: "hil-preempt", Version: 1, Tier: "required", Scope: "hil",
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
	definitions := completionTestCatalog{digest: digest, task: task}
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	// One run, claimed and carried as far as a single grounded step. Only
	// the flash step is reported: a preemption stops where it stopped, and
	// nothing demands a full set of steps from an attempt that was cut off.
	interrupted := func(t *testing.T) (string, string, BoardHILCompletion) {
		t.Helper()
		run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
			Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commit,
			SnapshotSHA256: strings.Repeat("d", 64), CatalogSHA256: digest,
			Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
				Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
		if err != nil {
			t.Fatal(err)
		}
		taskID := run.Tasks[0].ID
		assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
			testStart(taskID), definitions, commit)
		if err != nil || assignment == nil {
			t.Fatalf("claim HIL assignment: %v", err)
		}
		attemptID := assignment.Attempt.ID
		// ClaimNextBoardHILAttempt takes the next eligible HIL task on
		// this lease, which is not necessarily the one just created: a
		// task requeued by an earlier preemption is eligible too. Pin
		// that it took ours, so a stray claim fails here and not three
		// assertions later against another run's attempt.
		var claimedTask string
		if err := pool.QueryRow(ctx, "SELECT task_id::text FROM task_attempts WHERE id=$1",
			attemptID).Scan(&claimedTask); err != nil {
			t.Fatal(err)
		}
		if claimedTask != taskID {
			t.Fatalf("the claim took another task: want %s got %s", taskID, claimedTask)
		}
		segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			"flash", 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin flash segment: %v", err)
		}
		step := HILStep{Key: "flash", StartedAt: segment.StartedAt,
			EndedAt:    segment.StartedAt.Add(5 * time.Millisecond),
			DurationNS: (5 * time.Millisecond).Nanoseconds(), State: "failed"}
		time.Sleep(10 * time.Millisecond)
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attemptID, "failed"); err != nil {
			t.Fatalf("finish flash segment: %v", err)
		}
		return run.ID, taskID, BoardHILCompletion{AttemptID: attemptID, LeaseID: waiter.LeaseID,
			Generation: active.Generation, Result: "preempted", Steps: []HILStep{step}}
	}

	t.Run("a yield on a cancelled run ends it instead", func(t *testing.T) {
		// Nothing to come back to. The board reports the same thing it
		// would have reported anyway, and the plane rewrites it, rather
		// than parking a task on a run that is already going away.
		runID, _, completion := interrupted(t)
		if _, err := s.RequestRunCancellation(ctx, runID, "integration-human"); err != nil {
			t.Fatalf("cancel the run: %v", err)
		}
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, commit); err != nil {
			t.Fatalf("a preemption on a cancelled run was refused: %v", err)
		}
		var attemptState, taskState string
		if err := pool.QueryRow(ctx, `SELECT a.state,t.state FROM task_attempts a
			JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`,
			completion.AttemptID).Scan(&attemptState, &taskState); err != nil {
			t.Fatal(err)
		}
		if attemptState != "cancelled" || taskState != "cancelled" {
			t.Fatalf("a preemption on a cancelled run was parked: attempt=%q task=%q", attemptState, taskState)
		}
	})
	t.Run("a yield puts the task back in the queue", func(t *testing.T) {
		_, taskID, completion := interrupted(t)
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, completion, definitions, commit); err != nil {
			t.Fatalf("a preemption was refused: %v", err)
		}
		var attemptState, taskState string
		var started, ended *time.Time
		var steps int
		if err := pool.QueryRow(ctx, `SELECT a.state,t.state,t.started_at,t.ended_at,
			(SELECT count(*) FROM task_steps WHERE attempt_id=a.id)
			FROM task_attempts a JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`,
			completion.AttemptID).Scan(&attemptState, &taskState, &started, &ended, &steps); err != nil {
			t.Fatal(err)
		}
		if attemptState != "preempted" || taskState != "scheduled" {
			t.Fatalf("a yield did not requeue the task: attempt=%q task=%q", attemptState, taskState)
		}
		// The task has to look unstarted again, or the next attempt
		// inherits the timing of the one the human interrupted.
		if started != nil || ended != nil {
			t.Fatalf("the requeued task kept its old timing: started=%v ended=%v", started, ended)
		}
		if steps != 1 {
			t.Fatalf("the evidence the attempt did gather was not kept: steps=%d", steps)
		}
		var requeued bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM audit
			WHERE target_id=$1 AND action='task.requeued')`, taskID).Scan(&requeued); err != nil {
			t.Fatal(err)
		}
		if !requeued {
			t.Fatal("the requeue left no audit behind")
		}
	})

}
