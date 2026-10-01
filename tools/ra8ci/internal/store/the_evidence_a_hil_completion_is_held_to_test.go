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

// The band of refusals between the identity check (pinned already) and the
// writes: what the door demands of the work itself once it accepts who is
// reporting it.
//
// Each refusal here answers a different question, and the classes overlap
// (two conflicts, two invalids, one denial), so every case asserts the
// reason as well as the class. Four of the five need no board segments,
// because they are decided before any segment is looked up; the fifth is
// the one that asks for a segment and finds none.
func TestIntegrationTheEvidenceAHILCompletionIsHeldTo(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "hil-evidence-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "HIL evidence proof", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-evidence", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 60, BoardPolicy: "exclusive", HIL: hil,
		Steps: []catalog.Step{{Name: "flash", Program: "test-adapter"}, {Name: "observe", Program: "test-adapter"}},
		Retry: catalog.RetryPolicy{MaxAttempts: 1}}
	if err := catalog.ValidateTask(task); err != nil {
		t.Fatalf("invalid test HIL task: %v", err)
	}
	encode := func(h *catalog.HILTask) json.RawMessage {
		t.Helper()
		body, err := json.Marshal(h)
		if err != nil {
			t.Fatal(err)
		}
		out := append([]byte(`{"argv":[],"hil":`), body...)
		return json.RawMessage(append(out, byte(125)))
	}
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commit,
		SnapshotSHA256: strings.Repeat("d", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: encode(hil),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	taskID := run.Tasks[0].ID
	definitions := completionTestCatalog{digest: digest, task: task}
	assignment, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID,
		testStart(taskID), definitions, commit)
	if err != nil || assignment == nil {
		t.Fatalf("claim HIL assignment: %v", err)
	}
	attempt := assignment.Attempt

	// A step that is sound on its own terms: keyed to the first declared
	// step, inside the attempt's window, and consistent in its timing. The
	// only thing it lacks is a board segment to stand behind it.
	started := attempt.StartedAt.Add(time.Second)
	grounded := HILStep{Key: "flash", StartedAt: started, EndedAt: started.Add(5 * time.Millisecond),
		DurationNS: (5 * time.Millisecond).Nanoseconds(), State: "failed"}
	zero := 0
	base := BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "failed", Steps: []HILStep{grounded}}

	t.Run("the definition the run recorded changed underneath it", func(t *testing.T) {
		// The catalog still says one thing and the run's own arguments
		// now say another. Neither is authoritative over the other, so
		// the only safe answer is to refuse rather than pick one.
		drifted := *hil
		drifted.FlashRestoreSeconds = hil.FlashRestoreSeconds + 1
		original := encode(hil)
		if _, err := pool.Exec(ctx, "UPDATE tasks SET arguments=$1 WHERE id=$2", encode(&drifted), taskID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE tasks SET arguments=$1 WHERE id=$2", original, taskID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, base, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "persisted HIL definition changed") {
			t.Fatalf("a drifted definition was not refused: %v", err)
		}
	})

	t.Run("a board agent that never claimed this attempt", func(t *testing.T) {
		// A second agent on the same board, correct in every identity
		// the door reads, because none of them is the agent's own. What
		// it cannot produce is the claim.
		stranger := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
		err := s.CompleteBoardHILAttempt(ctx, stranger, base, definitions, commit)
		if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "did not claim") {
			t.Fatalf("an agent reported work it never claimed: %v", err)
		}
	})

	t.Run("a success carrying less evidence than the work declares", func(t *testing.T) {
		// Two steps are declared and one is reported. A failure may be
		// partial, because work stops where it broke; a success may not.
		in := base
		in.Result = "succeeded"
		in.ChildExitCode = &zero
		in.EvidenceComplete = true
		succeeded := grounded
		succeeded.State = "succeeded"
		succeeded.ChildExitCode = &zero
		in.Steps = []HILStep{succeeded}
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "lacks step evidence") {
			t.Fatalf("a short success was accepted: %v", err)
		}
	})

	t.Run("more steps than the work declares", func(t *testing.T) {
		in := base
		in.Steps = []HILStep{grounded, grounded, grounded}
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "unexpected steps") {
			t.Fatalf("a report longer than its definition was accepted: %v", err)
		}
	})

	t.Run("a step with no closed board segment behind it", func(t *testing.T) {
		// This is the one that matters most: the step is plausible in
		// every respect the report can assert about itself. What makes
		// it untrue is that the board never recorded a bounded segment
		// for it, and only the plane can see that.
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, base, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "durable board segment") {
			t.Fatalf("an ungrounded step was accepted: %v", err)
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
