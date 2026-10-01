//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The rows a lease leaves when it ends badly.
//
// A lease that is handed back cleanly records "released", and that path is
// already held. The two unhappy endings land a board in the same place,
// recovery required, by different roads: the holder ran out of time, or
// the board-side agent stopped answering. The typed rows have to keep
// them apart, because the end reason on the lease is what an operator
// reads to decide whether to chase a person or a board, and the session
// has to be closed either way or the next grant inherits an open one.
func TestIntegrationTheRowsALeaseLeavesWhenItEndsBadly(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	type ending struct {
		leaseState  string
		endReason   string
		leaseEnded  bool
		boardState  string
		needsRecov  bool
		sessionOpen int
	}
	readEnding := func(t *testing.T, pool *pgxpool.Pool, boardID, leaseID string) ending {
		t.Helper()
		var got ending
		if err := pool.QueryRow(ctx, `SELECT state,COALESCE(end_reason,''),ended_at IS NOT NULL
			FROM board_leases WHERE id=$1`, leaseID).
			Scan(&got.leaseState, &got.endReason, &got.leaseEnded); err != nil {
			t.Fatalf("no typed lease row: %v", err)
		}
		if err := pool.QueryRow(ctx, "SELECT state,recovery_required FROM boards WHERE id=$1",
			boardID).Scan(&got.boardState, &got.needsRecov); err != nil {
			t.Fatalf("no typed board row: %v", err)
		}
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM board_sessions
			WHERE board_id=$1 AND ended_at IS NULL`, boardID).Scan(&got.sessionOpen); err != nil {
			t.Fatal(err)
		}
		return got
	}

	t.Run("the holder ran out of time", func(t *testing.T) {
		s, pool, _, _, active, waiter, now := activeTestBoard(t, ctx)
		boardID := active.BoardID
		if got := readEnding(t, pool, boardID, waiter.LeaseID); got.leaseState != "active" ||
			got.leaseEnded || got.sessionOpen != 1 {
			t.Fatalf("the live lease did not start from the state this test assumes: %+v", got)
		}
		// Well past the minute the lease was granted for. Expiry is the
		// clock's decision, not a command anyone sends, so it arrives on
		// the next tick.
		expired, _, _ := s.TickBoard(ctx, boardID, active.Version, now.Add(2*time.Minute))
		if expired.Phase != board.RecoveryRequired {
			t.Fatalf("an overrun lease did not require recovery: %+v", expired)
		}
		got := readEnding(t, pool, boardID, waiter.LeaseID)
		if got.leaseState != "ended" || !got.leaseEnded || got.endReason != "expired" {
			t.Fatalf("the overrun lease was not recorded as expired: %+v", got)
		}
		if got.boardState != "recovery_required" || !got.needsRecov {
			t.Fatalf("the typed board did not follow the snapshot into recovery: %+v", got)
		}
		// The lease row is closed, but the session is not: a board in
		// recovery keeps its lease and its session as the evidence of
		// what was running when time ran out. They are closed when
		// recovery replaces the lease, not when the clock refuses it.
		if got.sessionOpen != 1 {
			t.Fatalf("expiry discarded the session the recovery reads: %+v", got)
		}
	})

	t.Run("the board-side agent stopped answering", func(t *testing.T) {
		s, pool, _, agent, active, waiter, now := activeTestBoard(t, ctx)
		boardID := active.BoardID
		// The same destination as expiry, reached by a different road, so
		// the end reason is the only thing that still says which.
		lost, _, err := s.ApplyBoardCommand(ctx, agent,
			board.AgentUnavailable{Reason: "heartbeat lost"},
			active.Version, nil, nil, now.Add(2*time.Second))
		if err != nil {
			t.Fatalf("the agent could not report itself unavailable: %v", err)
		}
		if lost.Phase != board.RecoveryRequired {
			t.Fatalf("a lost agent did not require recovery: %+v", lost)
		}
		got := readEnding(t, pool, boardID, waiter.LeaseID)
		if got.leaseState != "ended" || !got.leaseEnded || got.endReason != "recovery_required" {
			t.Fatalf("a lease lost to the agent was not told apart from an overrun one: %+v", got)
		}
		if got.boardState != "recovery_required" || !got.needsRecov {
			t.Fatalf("the typed board did not follow the snapshot into recovery: %+v", got)
		}
		if got.sessionOpen != 1 {
			t.Fatalf("a lost agent discarded the session the recovery reads: %+v", got)
		}
	})

	t.Run("a lease handed back cleanly", func(t *testing.T) {
		// The control. Same rows, same reads, and the reason has to differ
		// from both endings above, or the two assertions mean nothing.
		s, pool, human, _, active, waiter, now := activeTestBoard(t, ctx)
		boardID := active.BoardID
		receipt := "clean-release-" + mustID(t)
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if err != nil {
			t.Fatal(err)
		}
		released, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
			LeaseID: waiter.LeaseID, Generation: active.Generation,
		}, active.Version, &NeutralSubmission{ChallengeID: challenge.ID, Receipt: []byte(receipt)},
			exactNeutralVerifier{challenge: challenge, receipt: receipt}, now.Add(2*time.Second))
		if err != nil || released.Phase != board.Ready {
			t.Fatalf("a clean release failed: %+v %v", released, err)
		}
		got := readEnding(t, pool, boardID, waiter.LeaseID)
		if got.leaseState != "ended" || got.endReason != "released" {
			t.Fatalf("a clean release was not recorded as released: %+v", got)
		}
		if got.boardState != "available" || got.needsRecov {
			t.Fatalf("a released board was not left available: %+v", got)
		}
		// The contrast that makes the two above meaningful: a lease given
		// back has nothing to recover from, so its session is closed.
		if got.sessionOpen != 0 {
			t.Fatalf("a clean release left %d open sessions behind", got.sessionOpen)
		}
	})
}
