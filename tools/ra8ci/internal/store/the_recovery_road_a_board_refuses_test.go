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

// The recovery road, and the turns off it a board will not take.
//
// A board that has been left in an unknown state is only handed back
// once an operator has run an approved plan and a neutral party has
// vouched for the result. The happy path is already pinned elsewhere;
// what is pinned here is every way the road refuses: a plan with no
// identity, a proof asked for before the plan has started, a proof
// asked for after the plan has closed, and the quarantine an operator
// can impose and then recover from.
func TestIntegrationTheRecoveryRoadABoardRefuses(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	s, pool, human, _, active, waiter, now := activeTestBoard(t, ctx)
	boardID := active.BoardID
	operator := boardTestActor(t, ctx, s, pool, boardID, "human", "operator")

	version := func(t *testing.T) uint64 {
		t.Helper()
		snapshot, err := s.GetBoard(ctx, boardID)
		if err != nil {
			t.Fatal(err)
		}
		return snapshot.Version
	}

	// A release with no neutral proof at all is the ordinary way a board
	// arrives at recovery, so it is how this fixture gets there.
	required, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, now.Add(2*time.Second))
	if !board.IsCode(err, board.RecoveryNecessary) || required.Phase != board.RecoveryRequired {
		t.Fatalf("the board did not enter recovery: %+v %v", required, err)
	}

	t.Run("a recovery plan with no identity", func(t *testing.T) {
		for _, c := range []board.BeginRecovery{
			{Reason: "restore approved image"},
			{PlanID: "restore-" + mustID(t)},
		} {
			_, _, err := s.ApplyBoardCommand(ctx, operator, c, version(t), nil, nil, now.Add(3*time.Second))
			if err == nil || !strings.Contains(err.Error(), "recovery requires actor, plan, and reason") {
				t.Fatalf("a nameless recovery plan was started: %+v %v", c, err)
			}
		}
	})

	t.Run("a proof asked for before the plan has started", func(t *testing.T) {
		// The board is awaiting recovery, not under one. There is
		// nothing yet for a neutral party to vouch against.
		if _, err := s.IssueBoardNeutralChallenge(ctx, operator, version(t), "recovery"); !errors.Is(err, ErrDenied) {
			t.Fatalf("a challenge was issued before any plan: %v", err)
		}
	})

	planID := "restore-" + mustID(t)

	t.Run("a proof asked for after the plan has closed", func(t *testing.T) {
		recovering, _, err := s.ApplyBoardCommand(ctx, operator, board.BeginRecovery{
			PlanID: planID, Reason: "restore approved image",
		}, version(t), nil, nil, now.Add(4*time.Second))
		if err != nil || recovering.Phase != board.Recovering {
			t.Fatalf("the operator could not start the plan: %+v %v", recovering, err)
		}
		// Close the plan behind the board's back. The phase still says
		// Recovering, so only the plan row stands between the operator
		// and a proof bound to nothing.
		tag, err := pool.Exec(ctx, `UPDATE board_recovery_context SET ended_at=clock_timestamp()
			WHERE board_id=$1 AND ended_at IS NULL`, boardID)
		if err != nil || tag.RowsAffected() != 1 {
			t.Fatalf("the plan this board started is missing: %v", err)
		}
		_, err = s.IssueBoardNeutralChallenge(ctx, operator, version(t), "recovery")
		if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "no active recovery plan") {
			t.Fatalf("a challenge was issued against a closed plan: %v", err)
		}
		if _, err := pool.Exec(ctx, `UPDATE board_recovery_context SET ended_at=NULL
			WHERE board_id=$1`, boardID); err != nil {
			t.Fatal(err)
		}
		// With the plan open again the same request succeeds and carries
		// the plan it is bound to, so the refusal above was the plan and
		// nothing else.
		challenge, err := s.IssueBoardNeutralChallenge(ctx, operator, version(t), "recovery")
		if err != nil || challenge.RecoveryPlanID != planID {
			t.Fatalf("the reopened plan was not honoured: %+v %v", challenge, err)
		}
	})

	t.Run("the quarantine an operator imposes, and the plan it still owes", func(t *testing.T) {
		quarantined, _, err := s.ApplyBoardCommand(ctx, operator, board.Quarantine{
			Reason: "fixture suspected damaged",
		}, version(t), nil, nil, now.Add(5*time.Second))
		if err != nil || quarantined.Phase != board.Quarantined {
			t.Fatalf("the operator could not quarantine the board: %+v %v", quarantined, err)
		}
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM boards WHERE id=$1", boardID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "quarantined" {
			t.Fatalf("the typed board row says %q", state)
		}
		// The plan from the previous subtest is still open, and the
		// projection upserts a plan only over a closed one. So a second
		// plan on top of a live one is refused rather than quietly
		// replacing the plan an operator is still working to.
		_, _, err = s.ApplyBoardCommand(ctx, operator, board.BeginRecovery{
			PlanID: "restore-" + mustID(t), Reason: "replace fixture",
		}, version(t), nil, nil, now.Add(6*time.Second))
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "recovery plan projection") {
			t.Fatalf("a second plan was laid over a live one: %v", err)
		}
		var open int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM board_recovery_context
			WHERE board_id=$1 AND ended_at IS NULL AND plan_id=$2`, boardID, planID).Scan(&open); err != nil {
			t.Fatal(err)
		}
		if open != 1 {
			t.Fatalf("the refused plan disturbed the live one: %d rows still open for %s", open, planID)
		}
		// Quarantine itself is a stop, not a dead end: once the open plan
		// is closed, a fresh one starts from it.
		if _, err := pool.Exec(ctx, `UPDATE board_recovery_context SET ended_at=clock_timestamp()
			WHERE board_id=$1 AND ended_at IS NULL`, boardID); err != nil {
			t.Fatal(err)
		}
		recovering, _, err := s.ApplyBoardCommand(ctx, operator, board.BeginRecovery{
			PlanID: "restore-" + mustID(t), Reason: "replace fixture",
		}, version(t), nil, nil, now.Add(7*time.Second))
		if err != nil || recovering.Phase != board.Recovering {
			t.Fatalf("a quarantined board refused a new plan: %+v %v", recovering, err)
		}
	})
}
