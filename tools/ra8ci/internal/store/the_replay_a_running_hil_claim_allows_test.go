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

// The replay a running HIL claim allows.
//
// A board agent that loses the answer to its claim asks again, and the
// plane hands back the attempt already running under that lease rather
// than an empty queue. That replay is still a handover, so it is held
// to the same contract as the first claim: the agent must be running
// the trusted commit, the task's stored arguments must still match the
// catalog, and the timing pinned when the attempt started must still
// describe the task being replayed. Each of those failing is a
// conflict, never a quiet second claim.
func TestIntegrationTheReplayARunningHILClaimAllows(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	boardID := "hil-replay-" + mustID(t)
	holder := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	digest := strings.Repeat("1", 64)
	commitSHA := strings.Repeat("2", 40)
	now := time.Now().UTC()

	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: holder.ID(),
		Class: board.ClassAI, Reason: "a replay under test", Duration: 2 * time.Minute}
	pending, _, err := s.ApplyBoardCommand(ctx, holder, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatal(err)
	}
	if _, _, err := s.ApplyBoardCommand(ctx, boardAgent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: pending.Generation, InstalledGeneration: pending.Generation,
	}, pending.Version, nil, nil, now.Add(time.Second)); err != nil {
		t.Fatal(err)
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
	soundArguments := string(append(append([]byte(`{"argv":[],"hil":`), encodedHIL...), byte(125)))
	run, err := s.CreateRun(ctx, CreateRunInput{Trigger: "integration", ActorID: holder.ID(),
		Repository: boardTestRepo, Branch: "ci/ra8ci-implementation", CommitSHA: commitSHA,
		SnapshotSHA256: strings.Repeat("3", 64), CatalogSHA256: digest,
		Tasks: []TaskInput{{Key: "hil", Name: task.Name, Arguments: json.RawMessage(soundArguments),
			Tier: task.Tier, Scope: task.Scope, HostClass: "hil-lab", DeadlineSeconds: task.DeadlineSeconds}}})
	if err != nil {
		t.Fatal(err)
	}
	reviewed := queueTestCatalog{digest: digest, task: task}
	start := func() StartAttemptInput { return testStart(run.Tasks[0].ID) }

	first, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(), reviewed, commitSHA)
	if err != nil || first == nil {
		t.Fatalf("the first claim did not land: %v", err)
	}

	t.Run("a replay from an agent running another commit", func(t *testing.T) {
		replay, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			reviewed, strings.Repeat("4", 40))
		if replay != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "differs from the current reviewed catalog or trusted commit") {
			t.Fatalf("a running attempt was replayed to the wrong commit: %+v err=%v", replay, err)
		}
	})

	t.Run("a replay whose stored arguments have drifted", func(t *testing.T) {
		drifted := `{"argv":[],"hil":` + string(encodedHIL) + `,"rogue":1}`
		if _, err := pool.Exec(ctx, "UPDATE tasks SET arguments=$2::jsonb WHERE id=$1",
			run.Tasks[0].ID, drifted); err != nil {
			t.Fatal(err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, "UPDATE tasks SET arguments=$2::jsonb WHERE id=$1",
				run.Tasks[0].ID, soundArguments); err != nil {
				t.Fatal(err)
			}
		}()
		replay, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			reviewed, commitSHA)
		if replay != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "no longer matches its catalog contract") {
			t.Fatalf("an attempt whose arguments drifted was replayed: %+v err=%v", replay, err)
		}
	})

	t.Run("a replay whose reviewed board contract has moved", func(t *testing.T) {
		// The attempt was claimed against a ten second flash restore
		// bound. A catalog that now declares twenty describes different
		// hardware handling, so the running attempt no longer matches
		// its contract and the replay is refused rather than handed
		// over under terms nobody derived its timing from.
		moved := *hil
		moved.FlashRestoreSeconds = 20
		rebound := task
		rebound.HIL = &moved
		replay, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			queueTestCatalog{digest: digest, task: rebound}, commitSHA)
		if replay != nil || !errors.Is(err, ErrConflict) ||
			!strings.Contains(err.Error(), "no longer matches its catalog contract") {
			t.Fatalf("an attempt whose board contract moved was replayed: %+v err=%v", replay, err)
		}
	})

	t.Run("the replay the same agent is entitled to", func(t *testing.T) {
		replay, err := s.ClaimNextBoardHILAttempt(ctx, boardAgent, waiter.LeaseID, start(),
			reviewed, commitSHA)
		if err != nil || replay == nil {
			t.Fatalf("the sound replay was refused: %v", err)
		}
		if replay.Attempt.ID != first.Attempt.ID || replay.Attempt.AttemptNo != first.Attempt.AttemptNo {
			t.Fatalf("the replay started a second attempt: %+v", replay.Attempt)
		}
		if replay.HILTiming == nil || *replay.HILTiming != *first.HILTiming {
			t.Fatalf("the replay re-derived its timing: %+v", replay.HILTiming)
		}
		if replay.CommitSHA != commitSHA || replay.CatalogSHA256 != digest {
			t.Fatalf("the replay described other work: %+v", replay)
		}
		// However many times the claim is replayed, the lease holds one
		// attempt and the audit names one claim.
		var attempts, claims int
		if err := pool.QueryRow(ctx, `SELECT
			(SELECT count(*) FROM task_attempts WHERE board_lease_id=$1),
			(SELECT count(*) FROM audit WHERE action='board.hil.attempt_claimed'
			 AND reason->>'lease_id'=$2)`, waiter.LeaseID, waiter.LeaseID).Scan(&attempts, &claims); err != nil {
			t.Fatal(err)
		}
		if attempts != 1 || claims != 1 {
			t.Fatalf("the replays left %d attempts and %d claims", attempts, claims)
		}
	})
}
