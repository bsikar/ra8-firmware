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
)

// The fixture a neutral challenge insists on before it is issued.
//
// A proof is only worth anything if everyone agrees what was on the
// bench. So the challenge is built from the board's approved fixture
// profile, and the open session has to still match it. A board with no
// approved profile, or a session that has drifted from it, gets no
// challenge at all rather than one nobody can check.
func TestIntegrationTheFixtureANeutralChallengeInsistsOn(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()
	s, pool, human, _, active, _, _ := activeTestBoard(t, ctx)
	boardID := active.BoardID

	t.Run("a board with no approved fixture profile", func(t *testing.T) {
		var revision, profile, policy string
		if err := pool.QueryRow(ctx, `DELETE FROM board_fixture_profiles WHERE board_id=$1
			RETURNING fixture_revision,profile_sha256,restore_policy`, boardID).
			Scan(&revision, &profile, &policy); err != nil {
			t.Fatalf("the fixture this board was approved for is missing already: %v", err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, `INSERT INTO board_fixture_profiles
				(board_id,fixture_revision,profile_sha256,restore_policy) VALUES ($1,$2,$3,$4)`,
				boardID, revision, profile, policy); err != nil {
				t.Fatal(err)
			}
		}()
		_, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "no approved fixture profile") {
			t.Fatalf("an unapproved board was issued a challenge: %v", err)
		}
	})

	t.Run("a session that has drifted from the approved profile", func(t *testing.T) {
		var original string
		if err := pool.QueryRow(ctx, `UPDATE board_sessions SET fixture_revision='drifted-revision'
			WHERE board_id=$1 AND ended_at IS NULL RETURNING (SELECT fixture_revision
			FROM board_fixture_profiles WHERE board_id=$1)`, boardID).Scan(&original); err != nil {
			t.Fatalf("this board has no open session to drift: %v", err)
		}
		defer func() {
			if _, err := pool.Exec(ctx, `UPDATE board_sessions SET fixture_revision=$2
				WHERE board_id=$1 AND ended_at IS NULL`, boardID, original); err != nil {
				t.Fatal(err)
			}
		}()
		_, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "fixture session differs") {
			t.Fatalf("a drifted session was issued a challenge: %v", err)
		}
	})

	t.Run("the board as it was approved", func(t *testing.T) {
		// Both refusals above are repairs away from a working challenge,
		// so the same request must succeed once the rows are back.
		challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
		if err != nil {
			t.Fatalf("an approved, undrifted board was refused: %v", err)
		}
		var approved string
		if err := pool.QueryRow(ctx, `SELECT fixture_revision FROM board_fixture_profiles
			WHERE board_id=$1`, boardID).Scan(&approved); err != nil {
			t.Fatal(err)
		}
		if challenge.FixtureRevision != approved || challenge.ProfileSHA256 == "" {
			t.Fatalf("the challenge does not carry the approved fixture: %+v", challenge)
		}
	})
}
