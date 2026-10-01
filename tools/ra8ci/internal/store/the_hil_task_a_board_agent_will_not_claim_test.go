//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The HIL task a board agent will not claim.
//
// A board agent holding a live lease may only start the work that
// lease was granted for: a scheduled hardware task, on this board,
// belonging to the run its holder owns. Everything else is refused
// before an attempt row exists, because a started attempt is what
// puts hardware in motion. The closing case claims for real, so the
// refusals cannot be passing because the fixture was never claimable.
func TestIntegrationTheHILTaskABoardAgentWillNotClaim(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	agent := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	stranger := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: agent.ID(),
		Class: board.ClassAI, Reason: "a claim under test", Duration: time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, agent, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second)); err != nil {
		t.Fatal(err)
	}

	hilArgs := func(onBoard string) []byte {
		return []byte(`{"argv":[],"hil":{"board_id":"` + onBoard + `","board_model":"EK-RA8D2",` +
			`"manifest_path":"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",` +
			`"program_family":"uart-demo","mode":"uart_scrape","observation_step":"observe",` +
			`"flash_restore_seconds":10}}`)
	}
	runFor := func(t *testing.T, owner string, tasks []TaskInput) Run {
		t.Helper()
		run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: owner,
			Repository: boardTestRepo, CommitSHA: strings.Repeat("a", 40),
			SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: strings.Repeat("c", 64),
			Tasks: tasks})
		if err != nil {
			t.Fatal(err)
		}
		return run
	}
	hilTask := func(key, onBoard string) TaskInput {
		return TaskInput{Key: key, Name: "hil-run", Arguments: hilArgs(onBoard), Tier: "required",
			Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: 30}
	}

	held := runFor(t, agent.ID(), []TaskInput{
		hilTask("claimable", boardID),
		hilTask("finished", boardID),
		hilTask("another-board", "board-"+mustID(t)),
		{Key: "not-hardware", Name: "unit", Arguments: []byte(`{"argv":[]}`), Tier: "required",
			Scope: "runner", HostClass: "linux", DeadlineSeconds: 30},
	})

	t.Run("a task belonging to another holder's run", func(t *testing.T) {
		foreign := runFor(t, stranger.ID(), []TaskInput{hilTask("foreign", boardID)})
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, foreign.Tasks[0].ID, waiter.LeaseID,
			testStart(foreign.Tasks[0].ID))
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "no scheduled HIL task for active lease") {
			t.Fatalf("a board agent claimed work from a run its lease holder does not own: %v", err)
		}
	})

	t.Run("a task that is not hardware work", func(t *testing.T) {
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, held.Tasks[3].ID, waiter.LeaseID,
			testStart(held.Tasks[3].ID))
		if !errors.Is(err, ErrDenied) ||
			!strings.Contains(err.Error(), "cannot claim a non-HIL task") {
			t.Fatalf("a board agent claimed a task outside the hardware scope: %v", err)
		}
	})

	t.Run("a task the queue has already finished with", func(t *testing.T) {
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='succeeded',
			ended_at=clock_timestamp() WHERE id=$1`, held.Tasks[1].ID); err != nil {
			t.Fatal(err)
		}
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, held.Tasks[1].ID, waiter.LeaseID,
			testStart(held.Tasks[1].ID))
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "HIL task is succeeded") {
			t.Fatalf("a finished task was claimed again: %v", err)
		}
	})

	t.Run("a task whose hardware is a different board", func(t *testing.T) {
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, held.Tasks[2].ID, waiter.LeaseID,
			testStart(held.Tasks[2].ID))
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "does not match this board") {
			t.Fatalf("a board agent claimed another board's hardware task: %v", err)
		}
	})

	t.Run("a task under a run already being cancelled", func(t *testing.T) {
		cancelled := runFor(t, agent.ID(), []TaskInput{hilTask("cancelling", boardID)})
		// The pair constraint on runs keeps the request and its author
		// together, so a cancellation always names who asked for it.
		if _, err := pool.Exec(ctx, `UPDATE runs SET cancel_requested_at=clock_timestamp(),
			cancel_requested_by=$2 WHERE id=$1`, cancelled.ID, agent.ID()); err != nil {
			t.Fatal(err)
		}
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, cancelled.Tasks[0].ID, waiter.LeaseID,
			testStart(cancelled.Tasks[0].ID))
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "cancelled run cannot start another HIL task") {
			t.Fatalf("a cancelled run started new hardware work: %v", err)
		}
	})

	t.Run("the claim the same lease is entitled to", func(t *testing.T) {
		attempt, err := s.StartBoardHILAttempt(ctx, boardAgent, held.Tasks[0].ID, waiter.LeaseID,
			testStart(held.Tasks[0].ID))
		if err != nil {
			t.Fatalf("the claimable task was refused: %v", err)
		}
		if attempt.TaskID != held.Tasks[0].ID || attempt.State != "running" {
			t.Fatalf("the claim returned %+v", attempt)
		}
		// The deadline is derived here, never taken from the caller, so
		// it has to sit inside the task's own maximum.
		if !attempt.DeadlineAt.After(attempt.StartedAt) ||
			attempt.DeadlineAt.Sub(attempt.StartedAt) > 30*time.Second+time.Millisecond {
			t.Fatalf("the derived window ran outside the task maximum: %s to %s",
				attempt.StartedAt, attempt.DeadlineAt)
		}
		var claimed int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
			WHERE action='board.hil.attempt_claimed' AND target_type='attempt'
			AND target_id=$1 AND reason->>'lease_id'=$2`, attempt.ID, waiter.LeaseID).Scan(&claimed); err != nil {
			t.Fatal(err)
		}
		if claimed != 1 {
			t.Fatalf("the claim left %d audit rows naming the lease", claimed)
		}
	})
}
