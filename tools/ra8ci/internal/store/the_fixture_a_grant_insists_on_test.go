//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// TestIntegrationTheFixtureAGrantInsistsOn pins projectBoard's grant door:
// a lease is never handed out on a board that has no approved fixture
// profile, because the session it opens would have no restore policy to
// honour. The refusal is ErrDenied, not a conflict a caller would retry,
// and it takes the whole transition with it: no board, no waiter, no lease.
func TestIntegrationTheFixtureAGrantInsistsOn(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	boardID := "board-" + mustID(t)
	agent := boardTestActor(t, ctx, s, pool, boardID, "agent", "submitter")
	count := func(table string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM "+table+" WHERE board_id=$1", boardID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	enqueue := func() (board.Snapshot, error) {
		waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: agent.ID(),
			Class: board.ClassAI, Reason: "fixture door", Duration: time.Minute}
		after, _, err := s.ApplyBoardCommand(ctx, agent, board.Enqueue{Waiter: waiter}, 0, nil, nil, time.Now().UTC())
		return after, err
	}

	t.Run("a board with no approved fixture is never granted", func(t *testing.T) {
		if _, err := pool.Exec(ctx, "DELETE FROM board_fixture_profiles WHERE board_id=$1", boardID); err != nil {
			t.Fatal(err)
		}
		_, err := enqueue()
		if !errors.Is(err, ErrDenied) || errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "approved fixture required for grant") {
			t.Fatalf("grant without a fixture was not denied for that reason: %v", err)
		}
		if _, err := s.GetBoard(ctx, boardID); !errors.Is(err, ErrNotFound) {
			t.Fatalf("a denied grant left a board behind: %v", err)
		}
		for _, table := range []string{"board_snapshots", "board_waiters", "board_leases", "board_sessions"} {
			if n := count(table); n != 0 {
				t.Fatalf("a denied grant left %d rows in %s", n, table)
			}
		}
	})

	t.Run("the same grant lands once the fixture is approved", func(t *testing.T) {
		if _, err := pool.Exec(ctx, `INSERT INTO board_fixture_profiles
			(board_id,fixture_revision,profile_sha256,restore_policy)
			VALUES ($1,'fixture-v1',$2,'restore-image')`, boardID, strings.Repeat("a", 64)); err != nil {
			t.Fatal(err)
		}
		after, err := enqueue()
		if err != nil || after.Phase != board.GrantPending || after.Lease == nil {
			t.Fatalf("approved board was not granted: phase=%s err=%v", after.Phase, err)
		}
		var revision, policy, owner string
		if err := pool.QueryRow(ctx, `SELECT fixture_revision, restore_policy, owner_id FROM board_sessions
			WHERE board_id=$1 AND lease_id=$2`, boardID, after.Lease.ID).Scan(&revision, &policy, &owner); err != nil {
			t.Fatal(err)
		}
		if revision != "fixture-v1" || policy != "restore-image" || owner != agent.ID() {
			t.Fatalf("session did not carry the approved fixture: revision=%q policy=%q owner=%q", revision, policy, owner)
		}
		if n := count("board_leases"); n != 1 {
			t.Fatalf("granted board holds %d leases, want 1", n)
		}
	})
}
