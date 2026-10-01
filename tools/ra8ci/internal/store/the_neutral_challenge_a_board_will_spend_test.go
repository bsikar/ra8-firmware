//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"database/sql"
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The neutral challenge a board will issue, and what spending it costs.
//
// A release is only believed when a neutral party vouches that the board
// was left in its approved state, and the challenge is what binds that
// proof to one board, one lease, one snapshot version and one 30 second
// window. So the issuing door has to refuse anyone who is not the holder
// and any board that has moved on, and the spending door has to make a
// challenge single-use whether the proof it carried was good or not.
func TestIntegrationTheNeutralChallengeABoardWillSpend(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	s, pool, human, agent, active, waiter, _ := activeTestBoard(t, ctx)
	boardID := active.BoardID

	version := func(t *testing.T) uint64 {
		t.Helper()
		snapshot, err := s.GetBoard(ctx, boardID)
		if err != nil {
			t.Fatal(err)
		}
		return snapshot.Version
	}

	t.Run("a purpose the board issues no challenge for", func(t *testing.T) {
		for _, purpose := range []string{"", "audit", "RELEASE"} {
			_, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, purpose)
			if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "neutral challenge request") {
				t.Fatalf("purpose %q was accepted: %v", purpose, err)
			}
		}
		if _, err := s.IssueBoardNeutralChallenge(ctx, human, uint64(math.MaxInt64)+1, "release"); !errors.Is(err, ErrInvalid) {
			t.Fatalf("an out-of-range version was accepted: %v", err)
		}
	})

	t.Run("a board that has moved since the caller read it", func(t *testing.T) {
		_, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version+1, "release")
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "stale board version") {
			t.Fatalf("a stale version was issued a challenge: %v", err)
		}
	})

	t.Run("a release challenge for someone who is not the holder", func(t *testing.T) {
		// The board-side agent is a first-class actor on this board and
		// still cannot ask for the proof that would end someone else's
		// lease.
		if _, err := s.IssueBoardNeutralChallenge(ctx, agent, active.Version, "release"); !errors.Is(err, ErrDenied) {
			t.Fatalf("a non-holder was issued a release challenge: %v", err)
		}
	})

	t.Run("a recovery challenge from someone who is not an operator", func(t *testing.T) {
		if _, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "recovery"); !errors.Is(err, ErrDenied) {
			t.Fatalf("a holder was issued a recovery challenge: %v", err)
		}
	})

	t.Run("the challenge the holder is issued", func(t *testing.T) {
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, version(t), "release")
		if err != nil {
			t.Fatalf("the holder was refused a release challenge: %v", err)
		}
		if len(challenge.Nonce) != 64 || challenge.BoardID != boardID || challenge.LeaseID != waiter.LeaseID {
			t.Fatalf("the challenge does not name this lease: %+v", challenge)
		}
		var purpose, nonce string
		var leaseID sql.NullString
		var storedVersion int64
		var issued, expires time.Time
		var consumed sql.NullTime
		if err := pool.QueryRow(ctx, `SELECT purpose,nonce,lease_id,snapshot_version,issued_at,expires_at,consumed_at
			FROM board_neutral_challenges WHERE id=$1 AND board_id=$2`, challenge.ID, boardID).
			Scan(&purpose, &nonce, &leaseID, &storedVersion, &issued, &expires, &consumed); err != nil {
			t.Fatalf("the challenge was not persisted: %v", err)
		}
		if purpose != "release" || leaseID.String != waiter.LeaseID || consumed.Valid {
			t.Fatalf("the stored challenge does not match what was issued: %q %+v %+v", purpose, leaseID, consumed)
		}
		if uint64(storedVersion) != challenge.SnapshotVersion || nonce != challenge.Nonce {
			t.Fatalf("the stored challenge was rewritten: version=%d nonce=%q", storedVersion, nonce)
		}
		// The window is what keeps a proof from being prepared long in
		// advance and spent when the board is in another state entirely.
		if window := expires.Sub(issued); window != 30*time.Second {
			t.Fatalf("the challenge window is %s, not 30s", window)
		}
		var issuedAudit int
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit WHERE target_type='board'
			AND target_id=$1 AND action='board.neutral_challenge.issued'`, boardID).Scan(&issuedAudit); err != nil {
			t.Fatal(err)
		}
		if issuedAudit != 1 {
			t.Fatalf("the issued challenge left %d audit rows", issuedAudit)
		}
	})

	t.Run("a receipt the neutral party does not vouch for", func(t *testing.T) {
		// Its own board: an unvouched release is not a no-op, so nothing
		// after it could run against this one.
		s, pool, human, _, active, waiter, now := activeTestBoard(t, ctx)
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if err != nil {
			t.Fatal(err)
		}
		// The verifier is told to expect one receipt; the release carries
		// another. A holder claiming to have left the board in its
		// approved state, with nobody vouching for it, is exactly the
		// case the board cannot take on trust: it is not simply refused,
		// the board is put into recovery until someone looks at it.
		expected := "vouched-" + mustID(t)
		after, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
			LeaseID: waiter.LeaseID, Generation: active.Generation,
		}, active.Version, &NeutralSubmission{ChallengeID: challenge.ID, Receipt: []byte("not-the-receipt")},
			exactNeutralVerifier{challenge: challenge, receipt: expected}, now.Add(2*time.Second))
		if err == nil || !strings.Contains(err.Error(), "requires verified neutral receipt") {
			t.Fatalf("an unvouched receipt was taken at face value: %+v %v", after, err)
		}
		held, err := s.GetBoard(ctx, active.BoardID)
		if err != nil {
			t.Fatal(err)
		}
		if held.Phase != board.RecoveryRequired {
			t.Fatalf("an unvouched release left the board usable: %+v", held)
		}
		// The challenge is spent either way: a rejected proof must not
		// leave a window open for a second, better-prepared attempt.
		var outcome string
		var consumed sql.NullTime
		if err := pool.QueryRow(ctx, `SELECT COALESCE(outcome,''),consumed_at
			FROM board_neutral_challenges WHERE id=$1`, challenge.ID).Scan(&outcome, &consumed); err != nil {
			t.Fatal(err)
		}
		if outcome != "rejected" || !consumed.Valid {
			t.Fatalf("a rejected proof left the challenge spendable: %q %+v", outcome, consumed)
		}
	})

	t.Run("the challenge a good receipt spends", func(t *testing.T) {
		s, pool, human, _, active, waiter, now := activeTestBoard(t, ctx)
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if err != nil {
			t.Fatal(err)
		}
		receipt := "vouched-" + mustID(t)
		verifier := exactNeutralVerifier{challenge: challenge, receipt: receipt}
		released, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
			LeaseID: waiter.LeaseID, Generation: active.Generation,
		}, active.Version, &NeutralSubmission{ChallengeID: challenge.ID, Receipt: []byte(receipt)},
			verifier, now.Add(2*time.Second))
		if err != nil || released.Phase != board.Ready {
			t.Fatalf("a vouched release was refused: %+v %v", released, err)
		}
		var outcome string
		var consumed sql.NullTime
		if err := pool.QueryRow(ctx, `SELECT COALESCE(outcome,''),consumed_at
			FROM board_neutral_challenges WHERE id=$1`, challenge.ID).Scan(&outcome, &consumed); err != nil {
			t.Fatal(err)
		}
		if outcome != "accepted" || !consumed.Valid {
			t.Fatalf("the spent challenge was not closed: %q %+v", outcome, consumed)
		}
	})
}
