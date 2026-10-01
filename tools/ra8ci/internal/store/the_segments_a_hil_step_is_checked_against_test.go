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

// What a reported HIL step is checked against once a real board segment
// exists for it.
//
// The earlier slices stop where the plane starts comparing the report to
// what the board itself recorded. These three cases are that comparison: a
// step that disagrees with its segment's outcome, a step whose window does
// not fit inside its segment's, and a board still holding a segment open
// when the terminal report arrives.
//
// The cases run in order and share one board on purpose: the first two
// need the board quiet, and the third is the one that leaves a segment
// open, so it goes last.
func TestIntegrationTheSegmentsAHILStepIsCheckedAgainst(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "hil-segment-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "HIL segment proof", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-segment", Version: 1, Tier: "required", Scope: "hil",
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
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commit,
		SnapshotSHA256: strings.Repeat("d", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	definitions := completionTestCatalog{digest: digest, task: task}
	assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
		testStart(run.Tasks[0].ID), definitions, commit)
	if err != nil || assignment == nil {
		t.Fatalf("claim HIL assignment: %v", err)
	}
	attempt := assignment.Attempt
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	// One real, closed segment for the first declared step. It ended
	// failed, so a step reporting failure agrees with it and a step
	// reporting success does not.
	flash, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID, "flash", 10*time.Second, 0)
	if err != nil {
		t.Fatalf("begin flash segment: %v", err)
	}
	time.Sleep(10 * time.Millisecond)
	if err := s.FinishBoardSegment(ctx, boardAgent, flash.ID, token, attempt.ID, "failed"); err != nil {
		t.Fatalf("finish flash segment: %v", err)
	}

	agreeing := HILStep{Key: "flash", StartedAt: flash.StartedAt,
		EndedAt:    flash.StartedAt.Add(5 * time.Millisecond),
		DurationNS: (5 * time.Millisecond).Nanoseconds(), State: "failed"}
	base := BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "failed", Steps: []HILStep{agreeing}}

	t.Run("a step that disagrees with its segment", func(t *testing.T) {
		// The board recorded a failed segment. The report says the step
		// succeeded. Only one of those can be true, and the report is
		// not the side that gets to decide.
		succeeded := agreeing
		succeeded.State = "succeeded"
		zero := 0
		succeeded.ChildExitCode = &zero
		in := base
		in.Steps = []HILStep{succeeded}
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "contradicts its durable board segment") {
			t.Fatalf("a step that outran its segment's outcome was accepted: %v", err)
		}
	})

	t.Run("a step that ran past its segment", func(t *testing.T) {
		// Agreeing in outcome, but claiming work half a minute after the
		// board stopped bounding it. The attempt's own deadline is still
		// well clear, so only the segment can catch this.
		late := agreeing
		late.EndedAt = flash.StartedAt.Add(30 * time.Second)
		late.DurationNS = (30 * time.Second).Nanoseconds()
		in := base
		in.Steps = []HILStep{late}
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "contradicts its durable board segment") {
			t.Fatalf("a step reaching past its segment was accepted: %v", err)
		}
	})

	t.Run("a board still holding a segment open", func(t *testing.T) {
		// Everything about the report is now sound, including its one
		// grounded step. What is not sound is the board: it is still
		// inside a bounded segment, so the hardware is not known to be
		// back in a reportable state.
		open, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID, "observe", 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin observe segment: %v", err)
		}
		if open.ID == "" {
			t.Fatal("the open segment has no identity")
		}
		err = s.CompleteBoardHILAttempt(ctx, boardAgent, base, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "open board segment") {
			t.Fatalf("a terminal report was taken with the board still bounded: %v", err)
		}
	})

	var state string
	var steps int
	if err := pool.QueryRow(ctx, `SELECT a.state,
		(SELECT count(*) FROM task_steps WHERE attempt_id=a.id)
		FROM task_attempts a WHERE a.id=$1`, attempt.ID).Scan(&state, &steps); err != nil {
		t.Fatal(err)
	}
	if state != "running" || steps != 0 {
		t.Fatalf("a refused completion was written down: state=%q steps=%d", state, steps)
	}
}
