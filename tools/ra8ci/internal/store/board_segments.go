package store

import (
	"context"
	"fmt"
	"math"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
)

// BoardSegment is a durable, bounded interval of hardware access. Its
// timestamps are assigned by PostgreSQL, never by the requesting agent.
type BoardSegment struct {
	ID               string    `json:"id"`
	BoardID          string    `json:"board_id"`
	LeaseID          string    `json:"lease_id"`
	Generation       uint64    `json:"generation"`
	AttemptID        string    `json:"attempt_id"`
	Key              string    `json:"key"`
	StartedAt        time.Time `json:"started_at"`
	DeadlineAt       time.Time `json:"deadline_at"`
	RecoveryMarginMS uint64    `json:"recovery_margin_ms"`
}

// BeginBoardSegment serializes a bounded operation with lease transitions on
// the same advisory lock. If a human waiter wins that lock first, the active
// phase changes and this operation is denied. If this call wins, the durable
// segment records the operation that must finish before the next checkpoint.
func (s *Store) BeginBoardSegment(ctx context.Context, actor BoardActor, expectedVersion uint64, token board.Token, attemptID, key string, bound, recoveryMargin time.Duration) (BoardSegment, error) {
	if s == nil || s.pool == nil || actor.id == "" || actor.kind == "system" ||
		!validBoardID(actor.boardID) || expectedVersion > math.MaxInt64 ||
		token.BoardID != actor.boardID || !ValidID(attemptID) || !validSegmentKey(key) ||
		bound <= 0 || bound > 24*time.Hour || recoveryMargin < 0 || recoveryMargin > 24*time.Hour ||
		bound.Milliseconds() <= 0 || recoveryMargin.Milliseconds() < 0 {
		return BoardSegment{}, fmt.Errorf("%w: board segment arguments", ErrInvalid)
	}
	if token.Generation > math.MaxInt64 {
		return BoardSegment{}, fmt.Errorf("%w: board segment generation", ErrInvalid)
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.Serializable})
	if err != nil {
		return BoardSegment{}, fmt.Errorf("%w: begin board segment: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return BoardSegment{}, fmt.Errorf("%w: board lock: %v", ErrUnavailable, err)
	}
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return BoardSegment{}, err
	}
	snapshot, err := loadOrCreateBoard(ctx, tx, actor.boardID)
	if err != nil {
		return BoardSegment{}, err
	}
	if snapshot.Version != expectedVersion {
		return BoardSegment{}, fmt.Errorf("%w: stale board version", ErrConflict)
	}
	physicalAgent := actor.kind == "board_agent" && actor.role == "board_agent"
	if !ownsLease(snapshot, actor.id) && !physicalAgent {
		return BoardSegment{}, auditBoardDenial(ctx, tx, actor, ErrDenied)
	}
	now, err := databaseClock(ctx, tx)
	if err != nil {
		return BoardSegment{}, err
	}
	if err := board.CanStartSegment(snapshot, token, now, bound, recoveryMargin); err != nil {
		return BoardSegment{}, auditBoardDenial(ctx, tx, actor, err)
	}
	if actor.kind != "board_agent" || actor.role != "board_agent" {
		return BoardSegment{}, auditBoardDenial(ctx, tx, actor, ErrDenied)
	}
	if err := validateHILSegmentAttempt(ctx, tx, attemptID, token, bound); err != nil {
		return BoardSegment{}, auditBoardDenial(ctx, tx, actor, err)
	}
	var openSegment bool
	if err := tx.QueryRow(ctx, "SELECT EXISTS (SELECT 1 FROM board_segments WHERE board_id=$1 AND ended_at IS NULL)", actor.boardID).Scan(&openSegment); err != nil {
		return BoardSegment{}, fmt.Errorf("%w: check open board segment: %v", ErrUnavailable, err)
	}
	if openSegment {
		return BoardSegment{}, auditBoardDenial(ctx, tx, actor, fmt.Errorf("%w: prior segment is still open", ErrConflict))
	}
	id, err := NewID()
	if err != nil {
		return BoardSegment{}, fmt.Errorf("%w: segment ID: %v", ErrUnavailable, err)
	}
	segment := BoardSegment{ID: id, BoardID: actor.boardID, LeaseID: token.LeaseID,
		Generation: token.Generation, AttemptID: attemptID, Key: key, StartedAt: now, DeadlineAt: now.Add(bound),
		RecoveryMarginMS: uint64(recoveryMargin.Milliseconds())}
	_, err = tx.Exec(ctx, `INSERT INTO board_segments
		(id,board_id,lease_id,generation,attempt_id,actor_id,segment_key,started_at,deadline_at,recovery_margin_ms)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)`, segment.ID, segment.BoardID, segment.LeaseID,
		int64(segment.Generation), segment.AttemptID, actor.id, segment.Key, segment.StartedAt, segment.DeadlineAt,
		int64(segment.RecoveryMarginMS))
	if err != nil {
		return BoardSegment{}, fmt.Errorf("%w: persist board segment: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor.id, "board.segment.started", "board", actor.boardID,
		"ok", "", "", "", map[string]any{"segment_id": id, "lease_id": token.LeaseID, "generation": token.Generation, "attempt_id": attemptID, "holder": snapshot.Lease.Holder, "key": key, "deadline_at": segment.DeadlineAt}); err != nil {
		return BoardSegment{}, fmt.Errorf("%w: segment audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return BoardSegment{}, fmt.Errorf("%w: segment commit: %v", ErrUnavailable, err)
	}
	return segment, nil
}

// FinishBoardSegment closes only the segment created by the same authenticated
// physical board agent. A timed-out/open segment remains durable until explicitly
// reconciled; it is never silently cleared by a later lease.
func (s *Store) FinishBoardSegment(ctx context.Context, actor BoardActor, segmentID string, token board.Token, attemptID, outcome string) error {
	if s == nil || s.pool == nil || actor.id == "" || actor.kind == "system" ||
		!validBoardID(actor.boardID) || !validSegmentID(segmentID) || !ValidID(attemptID) || token.BoardID != actor.boardID ||
		(token.LeaseID == "" || token.Generation == 0) ||
		(outcome != "completed" && outcome != "failed" && outcome != "yielded") {
		return fmt.Errorf("%w: finish board segment arguments", ErrInvalid)
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.Serializable})
	if err != nil {
		return fmt.Errorf("%w: begin finish segment: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return fmt.Errorf("%w: board lock: %v", ErrUnavailable, err)
	}
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return err
	}
	var leaseID, actorID, storedAttemptID string
	var generation int64
	var ended bool
	err = tx.QueryRow(ctx, `SELECT lease_id::text, generation, actor_id, attempt_id::text, ended_at IS NOT NULL
		FROM board_segments WHERE id=$1 AND board_id=$2 FOR UPDATE`, segmentID, actor.boardID).
		Scan(&leaseID, &generation, &actorID, &storedAttemptID, &ended)
	if err != nil && err != pgx.ErrNoRows {
		return fmt.Errorf("%w: read board segment: %v", ErrUnavailable, err)
	}
	if err == pgx.ErrNoRows || leaseID != token.LeaseID || uint64(generation) != token.Generation || actorID != actor.id || storedAttemptID != attemptID || ended {
		return auditBoardDenial(ctx, tx, actor, ErrDenied)
	}
	_, err = tx.Exec(ctx, "UPDATE board_segments SET ended_at=clock_timestamp(), outcome=$2 WHERE id=$1", segmentID, outcome)
	if err != nil {
		return fmt.Errorf("%w: close board segment: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor.id, "board.segment.finished", "board", actor.boardID,
		"ok", "", "", "", map[string]any{"segment_id": segmentID, "lease_id": token.LeaseID, "generation": token.Generation, "attempt_id": attemptID, "outcome": outcome}); err != nil {
		return fmt.Errorf("%w: segment audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: finish segment commit: %v", ErrUnavailable, err)
	}
	return nil
}

func validSegmentKey(key string) bool {
	if key == "" || len(key) > 128 || strings.TrimSpace(key) != key {
		return false
	}
	for _, c := range key {
		if c < 0x20 || c == 0x7f {
			return false
		}
	}
	return true
}

func databaseClock(ctx context.Context, tx pgx.Tx) (time.Time, error) {
	var now time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&now); err != nil {
		return time.Time{}, fmt.Errorf("%w: read database clock: %v", ErrUnavailable, err)
	}
	return now.UTC(), nil
}

func validSegmentID(value string) bool { _, err := uuid.Parse(value); return err == nil }
