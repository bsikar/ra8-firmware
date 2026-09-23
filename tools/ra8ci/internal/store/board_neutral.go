package store

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5"
)

// neutralContext reads operator-approved fixture state and the pinned session.
// Neither a client nor the receipt verifier can select these values.
func neutralContext(ctx context.Context, tx pgx.Tx, snapshot board.Snapshot, purpose string) (NeutralChallenge, error) {
	c := NeutralChallenge{BoardID: snapshot.BoardID, Purpose: purpose, Generation: snapshot.Generation,
		SnapshotVersion: snapshot.Version, AgentHighWater: snapshot.AgentHighWater}
	if snapshot.Lease != nil {
		c.LeaseID = snapshot.Lease.ID
	}
	err := tx.QueryRow(ctx, `SELECT fixture_revision,profile_sha256,restore_policy
		FROM board_fixture_profiles WHERE board_id=$1`, snapshot.BoardID).
		Scan(&c.FixtureRevision, &c.ProfileSHA256, &c.RestorePolicy)
	if errors.Is(err, pgx.ErrNoRows) {
		return c, fmt.Errorf("%w: board has no approved fixture profile", ErrDenied)
	}
	if err != nil {
		return c, fmt.Errorf("%w: approved fixture read: %v", ErrUnavailable, err)
	}
	if snapshot.Lease != nil {
		var revision, profile, policy string
		err = tx.QueryRow(ctx, `SELECT fixture_revision,profile_sha256,restore_policy
			FROM board_sessions WHERE board_id=$1 AND lease_id=$2 AND ended_at IS NULL`,
			snapshot.BoardID, snapshot.Lease.ID).Scan(&revision, &profile, &policy)
		if errors.Is(err, pgx.ErrNoRows) || revision != c.FixtureRevision || profile != c.ProfileSHA256 || policy != c.RestorePolicy {
			return c, fmt.Errorf("%w: fixture session differs from approved profile", ErrDenied)
		}
		if err != nil {
			return c, fmt.Errorf("%w: fixture session read: %v", ErrUnavailable, err)
		}
	}
	if purpose == "release" {
		if snapshot.Lease == nil {
			return c, ErrDenied
		}
	} else if purpose == "recovery" {
		err = tx.QueryRow(ctx, `SELECT plan_id FROM board_recovery_context
			WHERE board_id=$1 AND ended_at IS NULL`, snapshot.BoardID).Scan(&c.RecoveryPlanID)
		if errors.Is(err, pgx.ErrNoRows) {
			return c, fmt.Errorf("%w: no active recovery plan", ErrDenied)
		}
		if err != nil {
			return c, fmt.Errorf("%w: recovery plan read: %v", ErrUnavailable, err)
		}
	} else {
		return c, ErrInvalid
	}
	return c, nil
}

// consumeNeutral verifies an exact, unexpired database challenge and consumes
// it in the caller's board transition transaction. A bad receipt is consumed
// as rejected; it can never be retried under a different fixture or version.
func consumeNeutral(ctx context.Context, tx pgx.Tx, snapshot board.Snapshot, purpose string, submission *NeutralSubmission, verifier NeutralReceiptVerifier) (string, error) {
	if submission == nil || verifier == nil || !ValidID(submission.ChallengeID) || len(submission.Receipt) == 0 || len(submission.Receipt) > 65536 {
		if err := appendAudit(ctx, tx, "ra8ci-server", "board.neutral_challenge.denied", "board", snapshot.BoardID,
			"denied", "", "", "", map[string]any{"reason": "missing or malformed proof", "purpose": purpose}); err != nil {
			return "", fmt.Errorf("%w: neutral denial audit: %v", ErrUnavailable, err)
		}
		return "", ErrDenied
	}
	var c NeutralChallenge
	var leaseID, planID sql.NullString
	var generation, version, highWater int64
	var consumedAt sql.NullTime
	err := tx.QueryRow(ctx, `SELECT id,nonce,board_id,purpose,lease_id,generation,snapshot_version,agent_high_water,
		fixture_revision,profile_sha256,restore_policy,recovery_plan_id,issued_at,expires_at,consumed_at
		FROM board_neutral_challenges WHERE id=$1 FOR UPDATE`, submission.ChallengeID).
		Scan(&c.ID, &c.Nonce, &c.BoardID, &c.Purpose, &leaseID, &generation, &version, &highWater,
			&c.FixtureRevision, &c.ProfileSHA256, &c.RestorePolicy, &planID, &c.IssuedAt, &c.ExpiresAt, &consumedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return "", ErrDenied
	}
	if err != nil {
		return "", fmt.Errorf("%w: neutral challenge read: %v", ErrUnavailable, err)
	}
	c.LeaseID, c.RecoveryPlanID = leaseID.String, planID.String
	c.Generation, c.SnapshotVersion, c.AgentHighWater = uint64(generation), uint64(version), uint64(highWater)
	if c.BoardID != snapshot.BoardID {
		return "", ErrDenied
	}
	var dbNow time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&dbNow); err != nil {
		return "", fmt.Errorf("%w: neutral clock: %v", ErrUnavailable, err)
	}
	valid := !consumedAt.Valid && !dbNow.Before(c.IssuedAt) && dbNow.Before(c.ExpiresAt)
	current, contextErr := neutralContext(ctx, tx, snapshot, purpose)
	if contextErr != nil {
		valid = false
	} else if c.BoardID != current.BoardID || c.Purpose != current.Purpose || c.LeaseID != current.LeaseID ||
		c.Generation != current.Generation || c.SnapshotVersion != current.SnapshotVersion ||
		c.AgentHighWater != current.AgentHighWater || c.FixtureRevision != current.FixtureRevision ||
		c.ProfileSHA256 != current.ProfileSHA256 || c.RestorePolicy != current.RestorePolicy ||
		c.RecoveryPlanID != current.RecoveryPlanID {
		valid = false
	}
	if valid && verifier.VerifyNeutralReceipt(ctx, c, submission.Receipt) != nil {
		valid = false
	}
	if consumedAt.Valid {
		return "", ErrDenied
	}
	sum := sha256.Sum256(submission.Receipt)
	result := "rejected"
	if valid {
		result = "accepted"
	}
	tag, err := tx.Exec(ctx, `UPDATE board_neutral_challenges SET consumed_at=clock_timestamp(),
		receipt_sha256=$2,outcome=$3 WHERE id=$1 AND consumed_at IS NULL`, c.ID, hex.EncodeToString(sum[:]), result)
	if err != nil || tag.RowsAffected() != 1 {
		return "", fmt.Errorf("%w: neutral challenge consume: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, "ra8ci-server", "board.neutral_challenge."+result,
		"board", snapshot.BoardID, result, "", "", "", map[string]any{"challenge_id": c.ID,
			"snapshot_version": c.SnapshotVersion, "purpose": purpose}); err != nil {
		return "", fmt.Errorf("%w: neutral challenge audit: %v", ErrUnavailable, err)
	}
	if !valid {
		return "", ErrDenied
	}
	return "sha256:" + hex.EncodeToString(sum[:]), nil
}

func auditBoardDenial(ctx context.Context, tx pgx.Tx, actor BoardActor, denial error) error {
	if err := appendAudit(ctx, tx, actor.id, "board.command.denied", "board", actor.boardID,
		"denied", "", "", "", map[string]any{"reason": denial.Error()}); err != nil {
		return fmt.Errorf("%w: board denial audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: board denial commit: %v", ErrUnavailable, err)
	}
	return denial
}
