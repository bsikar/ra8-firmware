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
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// registeredBoard plants the three rows a board needs to exist at all,
// with its operator-approved fixture. The fixture is written only when
// approved is set, so a board with no approved profile is reachable.
func registeredBoard(t *testing.T, ctx context.Context, pool *pgxpool.Pool, approved bool) string {
	t.Helper()
	boardID := "board-neutral-" + mustID(t)
	if _, err := pool.Exec(ctx, `INSERT INTO boards (id,generation,state,version)
		VALUES ($1,0,'available',0)`, boardID); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO board_snapshots (board_id,version,state)
		VALUES ($1,0,'{}'::jsonb)`, boardID); err != nil {
		t.Fatal(err)
	}
	if approved {
		if _, err := pool.Exec(ctx, `INSERT INTO board_fixture_profiles
			(board_id,fixture_revision,profile_sha256,restore_policy)
			VALUES ($1,'fixture-v1',$2,'restore-image')`, boardID, strings.Repeat("a", 64)); err != nil {
			t.Fatal(err)
		}
	}
	return boardID
}

// The context a neutral challenge is frozen to.
//
// A challenge is only worth anything because the state it names cannot
// be chosen by whoever answers it. Every piece of that context is read
// from the database here, and a board missing any of it has to fail shut
// rather than issue a challenge nobody can be held to.
func TestIntegrationTheContextANeutralChallengeIsFrozenTo(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	approved := registeredBoard(t, ctx, pool, true)
	snapshotOf := func(boardID string) board.Snapshot {
		return board.Snapshot{BoardID: boardID, Phase: board.Recovering,
			Generation: 3, AgentHighWater: 7, Version: 5}
	}

	t.Run("a board with no approved fixture profile", func(t *testing.T) {
		unapproved := registeredBoard(t, ctx, pool, false)
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			_, err := neutralContext(ctx, tx, snapshotOf(unapproved), "recovery")
			if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "no approved fixture profile") {
				t.Fatalf("an unapproved board was given a challenge context: %v", err)
			}
		})
	})

	t.Run("a release by a board holding no lease", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if _, err := neutralContext(ctx, tx, snapshotOf(approved), "release"); !errors.Is(err, ErrDenied) {
				t.Fatalf("a leaseless board was asked to attest a release: %v", err)
			}
		})
	})

	t.Run("a purpose the plane does not issue", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			if _, err := neutralContext(ctx, tx, snapshotOf(approved), "handover"); !errors.Is(err, ErrInvalid) {
				t.Fatalf("an unknown purpose was accepted: %v", err)
			}
		})
	})

	t.Run("a recovery with no active plan", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			_, err := neutralContext(ctx, tx, snapshotOf(approved), "recovery")
			if !errors.Is(err, ErrDenied) || !strings.Contains(err.Error(), "no active recovery plan") {
				t.Fatalf("a recovery challenge was issued with no plan: %v", err)
			}
		})
	})

	t.Run("the recovery context it does freeze", func(t *testing.T) {
		planned := registeredBoard(t, ctx, pool, true)
		if _, err := pool.Exec(ctx, `INSERT INTO board_recovery_context (board_id,plan_id)
			VALUES ($1,'plan-after-quarantine')`, planned); err != nil {
			t.Fatal(err)
		}
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			c, err := neutralContext(ctx, tx, snapshotOf(planned), "recovery")
			if err != nil {
				t.Fatalf("a planned recovery was refused a context: %v", err)
			}
			if c.RecoveryPlanID != "plan-after-quarantine" || c.FixtureRevision != "fixture-v1" ||
				c.ProfileSHA256 != strings.Repeat("a", 64) || c.RestorePolicy != "restore-image" {
				t.Fatalf("the fixture and plan were not frozen into the context: %+v", c)
			}
			// The state the answer will be held to comes from the locked
			// snapshot, never from the caller's request.
			if c.BoardID != planned || c.Purpose != "recovery" || c.LeaseID != "" ||
				c.Generation != 3 || c.SnapshotVersion != 5 || c.AgentHighWater != 7 {
				t.Fatalf("the context does not name the snapshot it was taken from: %+v", c)
			}
		})
	})
}

// The proof a neutral challenge will not accept.
//
// A receipt is answered against one exact stored challenge. A challenge
// that was never issued, or was issued for another board, must be
// refused before it is consumed, so the real one is still answerable.
func TestIntegrationTheProofANeutralChallengeWillNotAccept(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	elsewhere := registeredBoard(t, ctx, pool, true)
	here := registeredBoard(t, ctx, pool, true)
	snapshot := board.Snapshot{BoardID: here, Phase: board.Recovering, Version: 5}
	receipt := []byte("receipt bytes")

	t.Run("a proof that is missing or malformed", func(t *testing.T) {
		oversized := make([]byte, 65537)
		for name, submission := range map[string]*NeutralSubmission{
			"no submission at all":       nil,
			"no challenge identifier":    {ChallengeID: "not-a-uuid", Receipt: receipt},
			"no receipt":                 {ChallengeID: mustID(t)},
			"a receipt beyond the bound": {ChallengeID: mustID(t), Receipt: oversized},
		} {
			t.Run(name, func(t *testing.T) {
				inOwnTx(t, ctx, s, func(tx pgx.Tx) {
					proof, err := consumeNeutral(ctx, tx, snapshot, "recovery", submission, exactNeutralVerifier{})
					if !errors.Is(err, ErrDenied) || proof != "" {
						t.Fatalf("a malformed proof was accepted: %q %v", proof, err)
					}
					// The refusal is recorded, so a board agent cannot
					// probe the plane without leaving a trail.
					var denials int
					if err := tx.QueryRow(ctx, `SELECT count(*) FROM audit
						WHERE action='board.neutral_challenge.denied' AND target_type='board'
						AND target_id=$1`, here).Scan(&denials); err != nil {
						t.Fatal(err)
					}
					if denials != 1 {
						t.Fatalf("the denial left %d audit rows", denials)
					}
				})
			})
		}
	})

	t.Run("a verifier the plane was not given", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			proof, err := consumeNeutral(ctx, tx, snapshot, "recovery",
				&NeutralSubmission{ChallengeID: mustID(t), Receipt: receipt}, nil)
			if !errors.Is(err, ErrDenied) || proof != "" {
				t.Fatalf("a proof was consumed with no verifier: %q %v", proof, err)
			}
		})
	})

	t.Run("a challenge nobody issued", func(t *testing.T) {
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			proof, err := consumeNeutral(ctx, tx, snapshot, "recovery",
				&NeutralSubmission{ChallengeID: mustID(t), Receipt: receipt}, exactNeutralVerifier{})
			if !errors.Is(err, ErrDenied) || proof != "" {
				t.Fatalf("an unissued challenge was answered: %q %v", proof, err)
			}
		})
	})

	t.Run("a challenge issued for another board", func(t *testing.T) {
		challengeID := mustID(t)
		// The nonce is unique across the table and this database outlives
		// the test, so it has to be minted rather than written down.
		nonce := strings.ReplaceAll(mustID(t), "-", "") + strings.ReplaceAll(mustID(t), "-", "")
		if _, err := pool.Exec(ctx, `INSERT INTO board_neutral_challenges
			(id,nonce,board_id,purpose,generation,snapshot_version,agent_high_water,
			fixture_revision,profile_sha256,restore_policy,recovery_plan_id,issued_at,expires_at)
			VALUES ($1,$2,$3,'recovery',0,0,0,'fixture-v1',$4,'restore-image','plan-elsewhere',
			clock_timestamp(),clock_timestamp()+interval '30 seconds')`,
			challengeID, nonce, elsewhere, strings.Repeat("a", 64)); err != nil {
			t.Fatal(err)
		}
		inOwnTx(t, ctx, s, func(tx pgx.Tx) {
			proof, err := consumeNeutral(ctx, tx, snapshot, "recovery",
				&NeutralSubmission{ChallengeID: challengeID, Receipt: receipt}, exactNeutralVerifier{})
			if !errors.Is(err, ErrDenied) || proof != "" {
				t.Fatalf("another board's challenge was answered here: %q %v", proof, err)
			}
		})
		// Refused before consumption: the challenge the other board was
		// issued is still the one it can answer.
		var consumed, outcome *string
		if err := pool.QueryRow(ctx, `SELECT consumed_at::text,outcome FROM board_neutral_challenges
			WHERE id=$1`, challengeID).Scan(&consumed, &outcome); err != nil {
			t.Fatal(err)
		}
		if consumed != nil || outcome != nil {
			t.Fatalf("the other board's challenge was burned: consumed=%v outcome=%v", consumed, outcome)
		}
	})
}
