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

// What a board may say twice about an attempt it has already finished.
//
// A board agent that loses its answer on the wire has to be able to send the
// same completion again, or a flaky link turns into a stuck attempt. So the
// door accepts a repeat, but only a repeat: it reads back what was written
// the first time and refuses anything that differs, because the second
// report arriving is not evidence that the first one was wrong.
//
// The attempt here finishes failed, with both steps grounded in closed
// segments, and every case below is a second send against that record.
func TestIntegrationTheRetryAFinishedHILAttemptAllows(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "hil-retry-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "HIL retry proof", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-retry", Version: 1, Tier: "required", Scope: "hil",
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

	// Both declared steps, each inside its own closed segment. The
	// segments close failed, so steps reporting failure agree with them.
	steps := make([]HILStep, 0, len(task.Steps))
	for _, declared := range task.Steps {
		segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attempt.ID,
			declared.Name, 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin %s segment: %v", declared.Name, err)
		}
		steps = append(steps, HILStep{Key: declared.Name, StartedAt: segment.StartedAt,
			EndedAt:    segment.StartedAt.Add(5 * time.Millisecond),
			DurationNS: (5 * time.Millisecond).Nanoseconds(), State: "failed"})
		time.Sleep(10 * time.Millisecond)
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attempt.ID, "failed"); err != nil {
			t.Fatalf("finish %s segment: %v", declared.Name, err)
		}
	}
	reported := BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "failed", Steps: steps}
	if err := s.CompleteBoardHILAttempt(ctx, boardAgent, reported, definitions, commit); err != nil {
		t.Fatalf("the first completion was refused: %v", err)
	}

	t.Run("the same report again", func(t *testing.T) {
		// The answer the board never heard. Sending it again has to be
		// free, or a dropped reply strands the attempt.
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, reported, definitions, commit); err != nil {
			t.Fatalf("an identical retry was refused: %v", err)
		}
	})

	t.Run("a reason that was not there the first time", func(t *testing.T) {
		in := reported
		in.Reason = "board rebooted mid-run"
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "changed terminal result") {
			t.Fatalf("a retry rewrote the reason: %v", err)
		}
	})

	t.Run("an exit code that was not there the first time", func(t *testing.T) {
		// Not a worse result, just a more specific one. It is still a
		// different record from the one already written.
		one := 1
		in := reported
		in.ChildExitCode = &one
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "changed terminal result") {
			t.Fatalf("a retry added an exit code: %v", err)
		}
	})

	t.Run("a step whose evidence drifted", func(t *testing.T) {
		drifted := append([]HILStep(nil), steps...)
		drifted[1].DurationNS = steps[1].DurationNS * 2
		in := reported
		in.Steps = drifted
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "changed step evidence") {
			t.Fatalf("a retry rewrote step evidence: %v", err)
		}
	})

	t.Run("fewer steps than were written", func(t *testing.T) {
		in := reported
		in.Steps = steps[:1]
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "changed step count") {
			t.Fatalf("a retry dropped a step: %v", err)
		}
	})

	var state string
	var written int
	var duration int64
	if err := pool.QueryRow(ctx, `SELECT a.state,
		(SELECT count(*) FROM task_steps WHERE attempt_id=a.id),
		(SELECT duration_ns FROM task_steps WHERE attempt_id=a.id AND ordinal=1)
		FROM task_attempts a WHERE a.id=$1`, attempt.ID).Scan(&state, &written, &duration); err != nil {
		t.Fatal(err)
	}
	if state != "failed" || written != len(steps) || duration != steps[1].DurationNS {
		t.Fatalf("the retries moved the record: state=%q steps=%d duration=%d", state, written, duration)
	}
}
