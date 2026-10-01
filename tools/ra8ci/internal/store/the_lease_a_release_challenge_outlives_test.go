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

// TestIntegrationTheLeaseAReleaseChallengeMustPredate pins the release
// challenge's last door (board.go:151): a challenge is judged against the
// database clock, and one that would be issued at or after the lease's
// expiry is refused as a conflict rather than minted, so a holder cannot
// collect proof for a release after its lease has already lapsed. The
// lapsed lease is built honestly, by granting it at a caller time well in
// the past, so its expiry is half a minute behind the database before the
// challenge is ever asked for.
func TestIntegrationTheLeaseAReleaseChallengeMustPredate(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	held := func(t *testing.T, grantedAt time.Time) (BoardActor, string, uint64) {
		t.Helper()
		boardID := "board-" + mustID(t)
		human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
		agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
		waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(),
			Class: board.ClassHuman, Reason: "release challenge clock", Duration: time.Minute}
		granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, grantedAt)
		if err != nil {
			t.Fatal(err)
		}
		active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{
			LeaseID: waiter.LeaseID, Generation: granted.Generation, InstalledGeneration: granted.Generation,
		}, granted.Version, nil, nil, grantedAt.Add(time.Second))
		if err != nil || active.Phase != board.Active {
			t.Fatalf("lease was not installed: phase=%s err=%v", active.Phase, err)
		}
		return human, boardID, active.Version
	}
	challenges := func(t *testing.T, boardID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM board_neutral_challenges WHERE board_id=$1", boardID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}

	t.Run("a lease that lapsed before the challenge is refused", func(t *testing.T) {
		human, boardID, version := held(t, time.Now().UTC().Add(-90*time.Second))
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, version, "release")
		if !errors.Is(err, ErrConflict) || errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "lease expired before challenge") {
			t.Fatalf("challenge after the lease lapsed was not refused for that reason: %+v %v", challenge, err)
		}
		if challenge != (NeutralChallenge{}) {
			t.Fatalf("refused challenge handed back material: %+v", challenge)
		}
		if n := challenges(t, boardID); n != 0 {
			t.Fatalf("refused challenge persisted %d rows", n)
		}
	})

	t.Run("a live lease is challenged", func(t *testing.T) {
		human, boardID, version := held(t, time.Now().UTC())
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, version, "release")
		if err != nil || challenge.ID == "" || len(challenge.Nonce) != 64 {
			t.Fatalf("live lease was not challenged: %+v %v", challenge, err)
		}
		if n := challenges(t, boardID); n != 1 {
			t.Fatalf("issued challenge persisted %d rows, want 1", n)
		}
	})
}
