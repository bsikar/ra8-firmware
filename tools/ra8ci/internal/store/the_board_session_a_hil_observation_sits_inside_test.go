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

// The board session a HIL observation has to sit inside.
//
// An observation is only comparable with history if the plane knows which
// fixture produced it, and that identity comes from the board session the
// observation ran under, not from anything the agent reports. So the door
// looks the session up by the observation segment's own window and refuses
// a completion whenever that lookup is not unambiguous: a session that had
// already closed, one with no approved profile, two that both enclose the
// window, or an identity that no longer validates. Each refusal has to
// leave the attempt running, because an observation filed against the wrong
// fixture poisons every later comparison in that cohort.
func TestIntegrationTheBoardSessionAHILObservationSitsInside(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	boardID := "obs-session-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("e", 64)
	commit := strings.Repeat("a", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "observation session proof", Duration: 3 * time.Minute}
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
	task := catalog.Task{Name: "obs-session", Version: 1, Tier: "required", Scope: "hil",
		OS: []string{"linux"}, DeadlineSeconds: 90, BoardPolicy: "exclusive", HIL: hil,
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
		SnapshotSHA256: strings.Repeat("f", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(arguments),
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
	attemptID := assignment.Attempt.ID
	token := board.Token{BoardID: boardID, LeaseID: waiter.LeaseID, Generation: active.Generation}

	// Each declared step gets its own bounded segment, closed as completed,
	// and the step evidence is then read back off the segment's own window so
	// nothing in this test depends on a clock the plane did not write.
	bounded := func(key string) (time.Time, time.Time) {
		t.Helper()
		segment, err := s.BeginBoardSegment(ctx, boardAgent, active.Version, token, attemptID,
			key, 10*time.Second, 0)
		if err != nil {
			t.Fatalf("begin %s segment: %v", key, err)
		}
		if err := s.FinishBoardSegment(ctx, boardAgent, segment.ID, token, attemptID, "completed"); err != nil {
			t.Fatalf("finish %s segment: %v", key, err)
		}
		var started, ended time.Time
		if err := pool.QueryRow(ctx, "SELECT started_at,ended_at FROM board_segments WHERE id=$1",
			segment.ID).Scan(&started, &ended); err != nil {
			t.Fatal(err)
		}
		return started, ended
	}
	flashStart, flashEnd := bounded("flash")
	observeStart, observeEnd := bounded("observe")

	zero := 0
	step := func(key string, started, ended time.Time) HILStep {
		return HILStep{Key: key, StartedAt: started, EndedAt: ended,
			DurationNS: ended.Sub(started).Nanoseconds(), State: "succeeded", ChildExitCode: &zero}
	}
	sound := BoardHILCompletion{AttemptID: attemptID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "succeeded", ChildExitCode: &zero, EvidenceComplete: true,
		Steps: []HILStep{step("flash", flashStart, flashEnd), step("observe", observeStart, observeEnd)}}

	var sessionID, profile, revision string
	if err := pool.QueryRow(ctx, `SELECT id::text,profile_sha256,fixture_revision
		FROM board_sessions WHERE lease_id=$1 AND board_id=$2`, waiter.LeaseID, boardID).
		Scan(&sessionID, &profile, &revision); err != nil {
		t.Fatalf("the grant did not record a board session: %v", err)
	}

	stillRunning := func(t *testing.T) {
		t.Helper()
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1",
			attemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "running" {
			t.Fatalf("a refused completion ended the attempt anyway: %s", state)
		}
	}

	t.Run("a session that closed before the observation did", func(t *testing.T) {
		if _, err := pool.Exec(ctx, "UPDATE board_sessions SET ended_at=$1 WHERE id=$2",
			observeEnd.Add(-time.Second), sessionID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE board_sessions SET ended_at=NULL WHERE id=$1",
				sessionID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "outside its recorded board session") {
			t.Fatalf("an observation past its session was accepted: %v", err)
		}
		stillRunning(t)
	})

	t.Run("a session carrying no approved fixture profile", func(t *testing.T) {
		// Without the profile digest there is no cohort to compare the
		// observation against, so the session stops counting as one.
		if _, err := pool.Exec(ctx, "UPDATE board_sessions SET profile_sha256=NULL WHERE id=$1",
			sessionID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE board_sessions SET profile_sha256=$1 WHERE id=$2",
				profile, sessionID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "no board session encloses") {
			t.Fatalf("an observation with no fixture identity was accepted: %v", err)
		}
		stillRunning(t)
	})

	t.Run("an identity that no longer validates", func(t *testing.T) {
		// The revision is still present and still non-empty, and is still
		// refused, because a padded revision would group this observation
		// under a cohort name no other run would ever produce.
		if _, err := pool.Exec(ctx, "UPDATE board_sessions SET fixture_revision=$1 WHERE id=$2",
			" "+revision, sessionID); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE board_sessions SET fixture_revision=$1 WHERE id=$2",
				revision, sessionID); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit)
		if !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "invalid HIL board session identity") {
			t.Fatalf("an unvalidated session identity was accepted: %v", err)
		}
		stillRunning(t)
	})

	t.Run("two sessions enclosing the same observation", func(t *testing.T) {
		// Two candidates is not better evidence than one. The door cannot
		// tell which fixture was actually on the bench, so it refuses
		// rather than taking the most recent.
		//
		// The second session is planted already closed, past the end of
		// the observation: board_one_active_session_idx permits only one
		// open session per board, and a closed session that still spans
		// the window is read by the lookup just the same.
		second := mustID(t)
		if _, err := pool.Exec(ctx, `INSERT INTO board_sessions
			(id,board_id,lease_id,owner_id,fixture_revision,profile_sha256,phase,restore_policy,metadata_version,started_at,ended_at)
			SELECT $1,board_id,lease_id,owner_id,fixture_revision,profile_sha256,phase,restore_policy,1,started_at,$3
			FROM board_sessions WHERE id=$2`, second, sessionID, observeEnd.Add(time.Minute)); err != nil {
			t.Fatalf("plant a second enclosing session: %v", err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "DELETE FROM board_sessions WHERE id=$1", second); err != nil {
				t.Fatal(err)
			}
		}()
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "ambiguous board session") {
			t.Fatalf("an ambiguous session was accepted: %v", err)
		}
		stillRunning(t)
	})

	t.Run("the observation inside its own session", func(t *testing.T) {
		// The control: with the session exactly as the grant wrote it, the
		// same report the four refusals rejected is accepted, which is what
		// keeps those refusals from passing for some unrelated reason.
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit); err != nil {
			t.Fatalf("a grounded observation was refused: %v", err)
		}
		var attemptState, taskState string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1",
			attemptID).Scan(&attemptState); err != nil {
			t.Fatal(err)
		}
		if err := pool.QueryRow(ctx, "SELECT state FROM tasks WHERE id=$1", taskID).Scan(&taskState); err != nil {
			t.Fatal(err)
		}
		if attemptState != "succeeded" || taskState != "succeeded" {
			t.Fatalf("the completion did not finish the work: attempt=%s task=%s", attemptState, taskState)
		}
		var finished bool
		if err := pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM run_events
			WHERE run_id=$1 AND kind='attempt.finished')`, run.ID).Scan(&finished); err != nil {
			t.Fatal(err)
		}
		if !finished {
			t.Fatal("the completion left no attempt.finished event on the run")
		}
	})
}
