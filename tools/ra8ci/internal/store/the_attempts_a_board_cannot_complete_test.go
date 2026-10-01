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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// The attempts a board agent cannot report a terminal result for, taken
// after its arguments are read and before any evidence is written.
//
// The argument shapes are already pinned against a plane that points
// nowhere (what_the_board_doors_judge_before_the_database_test.go), and the
// whole happy path with its idempotent replay is pinned in
// board_hil_completion_integration_test.go. What is missing is the pair in
// between: a sound report naming work this door does not own. Both answer
// ErrNotFound, but they reach it from different sides of the run lock, and
// the second is the one that matters, because the attempt it names is real
// and must come back untouched.
func TestIntegrationTheAttemptsABoardCannotComplete(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID := "hil-refusal-" + mustID(t)
	boardAgent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	definitions := completionTestCatalog{
		digest: strings.Repeat("c", 64),
		task:   catalog.Task{Name: "hil-integration"},
	}
	commit := strings.Repeat("b", 40)

	t.Run("an attempt nobody has heard of", func(t *testing.T) {
		// Sound in every argument, so the door opens, takes its lock and
		// goes looking for the run. There is nothing to find.
		in := aHILCompletion()
		in.AttemptID = mustID(t)
		err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit)
		if !errors.Is(err, ErrNotFound) {
			t.Fatalf("an unknown attempt was not reported missing: %v", err)
		}
		if errors.Is(err, ErrInvalid) {
			t.Fatalf("the refusal was read as a shape problem: %v", err)
		}
	})

	t.Run("an attempt that was never run on a board", func(t *testing.T) {
		// An ordinary attempt, started the ordinary way with no board
		// lease behind it. Its run resolves, so this gets past the first
		// lookup and the run lock, and is then refused because the
		// attempt has no lease for the door to be bound to.
		run, err := s.CreateRun(ctx, testRun())
		if err != nil {
			t.Fatal(err)
		}
		taskID := run.Tasks[0].ID
		attempt, err := s.StartAttempt(ctx, testStart(taskID))
		if err != nil {
			t.Fatal(err)
		}

		in := aHILCompletion()
		in.AttemptID = attempt.ID
		if err := s.CompleteBoardHILAttempt(ctx, boardAgent, in, definitions, commit); !errors.Is(err, ErrNotFound) {
			t.Fatalf("a board agent completed an attempt that never touched a board: %v", err)
		}

		// The refusal must leave the attempt exactly as it was. A door
		// that wrote a terminal result first and refused afterwards
		// would end somebody else's work on a board it never ran on.
		var state string
		var evidence bool
		var reason *string
		if err := pool.QueryRow(ctx, `SELECT state, evidence_complete, result_reason
			FROM task_attempts WHERE id=$1`, attempt.ID).Scan(&state, &evidence, &reason); err != nil {
			t.Fatal(err)
		}
		if state != "running" || evidence || reason != nil {
			t.Fatalf("the refused attempt was written to: state=%q evidence=%v reason=%v", state, evidence, reason)
		}
		var steps int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM task_steps WHERE attempt_id=$1", attempt.ID).Scan(&steps); err != nil {
			t.Fatal(err)
		}
		if steps != 0 {
			t.Fatalf("the refused report left %d steps behind", steps)
		}
	})
}
