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

// The identity a HIL completion has to match before any evidence is read.
//
// The door reads the attempt, its lease and the run together, then holds
// the report against all three at once. Every spoiled case below is sound
// in every other respect, so each one proves its own clause is doing work:
// a chain of ors like this one is exactly where a dropped clause hides,
// because the obvious cases keep failing for the other reasons.
//
// All of these are refused before the claim audit and the step evidence
// are looked at, so no board segments are needed, and because each refusal
// rolls its transaction back the one claimed attempt serves every case.
func TestIntegrationTheIdentityAHILCompletionMustMatch(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	boardID := "hil-identity-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("c", 64)
	commit := strings.Repeat("b", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "HIL identity proof", Duration: 2 * time.Minute}
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
	task := catalog.Task{Name: "hil-identity", Version: 1, Tier: "required", Scope: "hil",
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

	// Sound in every respect the identity check reads.
	sound := BoardHILCompletion{AttemptID: attempt.ID, LeaseID: waiter.LeaseID,
		Generation: active.Generation, Result: "failed",
		Steps: []HILStep{{Key: "flash", State: "failed"}}}

	// A definition pinned to somebody else's board.
	elsewhere := *hil
	elsewhere.BoardID = "hil-other-" + mustID(t)
	otherBoardTask := task
	otherBoardTask.HIL = &elsewhere

	// A definition whose bound is not the one the run recorded.
	slowerTask := task
	slowerTask.DeadlineSeconds = task.DeadlineSeconds + 1

	// A catalog that knows some other work by this digest.
	strangerTask := task
	strangerTask.Name = "hil-something-else"

	for _, spoiled := range []struct {
		what        string
		in          BoardHILCompletion
		definitions HILDefinitionCatalog
		commit      string
	}{
		{"a generation that was never granted", func() BoardHILCompletion {
			in := sound
			in.Generation = active.Generation + 1
			return in
		}(), definitions, commit},
		{"another lease than the attempt was claimed under", func() BoardHILCompletion {
			in := sound
			in.LeaseID = mustID(t)
			return in
		}(), definitions, commit},
		{"a catalog that is not the one the run pinned", sound,
			completionTestCatalog{digest: strings.Repeat("e", 64), task: task}, commit},
		{"a commit that is not the one the run named", sound, definitions, strings.Repeat("f", 40)},
		{"a catalog that does not know this work", sound,
			completionTestCatalog{digest: digest, task: strangerTask}, commit},
		{"a definition pinned to another board", sound,
			completionTestCatalog{digest: digest, task: otherBoardTask}, commit},
		{"a definition whose deadline is not the task's", sound,
			completionTestCatalog{digest: digest, task: slowerTask}, commit},
	} {
		t.Run(spoiled.what, func(t *testing.T) {
			err := s.CompleteBoardHILAttempt(ctx, boardAgent, spoiled.in, spoiled.definitions, spoiled.commit)
			if !errors.Is(err, ErrConflict) {
				t.Fatalf("the door took %s: %v", spoiled.what, err)
			}
			if !strings.Contains(err.Error(), "identity mismatch") {
				t.Fatalf("%s was refused for another reason: %v", spoiled.what, err)
			}
		})
	}

	t.Run("the identity it was claimed under is not refused", func(t *testing.T) {
		// The anti-vacuity case. Nothing above is wrong here, so the
		// report gets past the identity check and is judged on its
		// evidence instead: the single step carries no timing, which
		// is a later and different refusal.
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, sound, definitions, commit)
		if err == nil {
			t.Fatal("a report with no step timing was accepted")
		}
		if strings.Contains(err.Error(), "identity mismatch") {
			t.Fatalf("the claimed identity was refused as a mismatch: %v", err)
		}
	})

	// Every case above rolled back. The attempt must still be the running,
	// unreported attempt the board agent claimed.
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
