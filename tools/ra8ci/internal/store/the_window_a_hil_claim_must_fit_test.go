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

// The window a HIL claim must fit, and the task it must not take over.
//
// The plane derives the hardware window itself from the manifest and
// what the board has done before, then checks that window against the
// maximum the task was planned with. A task whose maximum is shorter
// than the work needs is refused outright rather than started and cut
// off mid-flash. Separately, a task the queue already shows as running
// is not taken over by a claim that cannot show it owns that run.
func TestIntegrationTheWindowAHILClaimMustFit(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "hil-window-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "a window under test", Duration: 2 * time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second)); err != nil {
		t.Fatal(err)
	}

	arguments := []byte(`{"argv":[],"hil":{"board_id":"` + boardID + `","board_model":"EK-RA8D2",` +
		`"manifest_path":"examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",` +
		`"program_family":"uart-demo","mode":"uart_scrape","observation_step":"observe",` +
		`"flash_restore_seconds":10}}`)
	hilTask := func(key string, deadline int) TaskInput {
		return TaskInput{Key: key, Name: "hil-window", Arguments: arguments, Tier: "required",
			Scope: "hil", HostClass: "hil-lab", DeadlineSeconds: deadline}
	}
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, CommitSHA: strings.Repeat("7", 40),
		SnapshotSHA256: strings.Repeat("8", 64), CatalogSHA256: strings.Repeat("9", 64),
		Tasks: []TaskInput{hilTask("too-short", 1), hilTask("taken", 30), hilTask("sound", 30)}})
	if err != nil {
		t.Fatal(err)
	}

	t.Run("a task whose maximum is shorter than the work", func(t *testing.T) {
		// The demo manifest's fallback window is thirty seconds, so a
		// task planned with a one second maximum cannot hold it.
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[0].ID, waiter.LeaseID,
			testStart(run.Tasks[0].ID))
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "HIL timing exceeds task maximum") {
			t.Fatalf("a task too short for its own work was started: %v", err)
		}
	})

	t.Run("a task the queue already shows as running", func(t *testing.T) {
		// Nothing claimed this task through the plane, so there is no
		// attempt and no audit naming this lease. The claim must not
		// adopt the run on the strength of the task's state alone.
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='running',
			started_at=clock_timestamp() WHERE id=$1`, run.Tasks[1].ID); err != nil {
			t.Fatal(err)
		}
		_, err := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[1].ID, waiter.LeaseID,
			testStart(run.Tasks[1].ID))
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "already running under another claim") {
			t.Fatalf("a claim adopted a run it could not show it owned: %v", err)
		}
	})

	t.Run("nothing was started while the claims were refused", func(t *testing.T) {
		var attempts int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM task_attempts WHERE board_lease_id=$1",
			waiter.LeaseID).Scan(&attempts); err != nil {
			t.Fatal(err)
		}
		if attempts != 0 {
			t.Fatalf("the refused claims left %d attempts behind", attempts)
		}
	})

	t.Run("the task whose maximum holds the derived window", func(t *testing.T) {
		attempt, err := s.StartBoardHILAttempt(ctx, boardAgent, run.Tasks[2].ID, waiter.LeaseID,
			testStart(run.Tasks[2].ID))
		if err != nil {
			t.Fatalf("the sound task was refused: %v", err)
		}
		// The start and the deadline are two separate clock reads
		// rounded to microseconds, so the window can land a tick over
		// the maximum. A tick is rounding; a second is a bug.
		window := attempt.DeadlineAt.Sub(attempt.StartedAt)
		if window <= 0 || window > 30*time.Second+time.Millisecond {
			t.Fatalf("the derived window did not fit the task maximum: %s", window)
		}
		// The window is the plane's, so it is written down where a
		// later pass can check what was agreed.
		var pinned int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
			WHERE action='task.hil_timing_selected' AND target_type='attempt'
			AND target_id=$1`, attempt.ID).Scan(&pinned); err != nil {
			t.Fatal(err)
		}
		if pinned != 1 {
			t.Fatalf("the derived window was pinned %d times", pinned)
		}
	})
}
