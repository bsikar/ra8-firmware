package store

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

// StartBoardHILAttempt lets only the authenticated board agent start a queued
// HIL task for the principal that currently holds this board's lease. Host
// measurements are supplied by the board agent; lease identity is DB-derived.
func (s *Store) StartBoardHILAttempt(ctx context.Context, actor BoardActor, taskID, leaseID string, facts StartAttemptInput) (Attempt, error) {
	if s == nil || s.pool == nil || actor.kind != "board_agent" || actor.role != "board_agent" ||
		!validBoardID(actor.boardID) || !ValidID(taskID) || !ValidID(leaseID) {
		return Attempt{}, fmt.Errorf("%w: board HIL claim arguments", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: begin board HIL claim: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return Attempt{}, err
	}
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return Attempt{}, fmt.Errorf("%w: board HIL claim lock: %v", ErrUnavailable, err)
	}
	var holderID, scope, taskState string
	var cancelled bool
	var rawArguments []byte
	err = tx.QueryRow(ctx, `SELECT l.holder_id,t.scope,t.state,t.arguments,(r.cancel_requested_at IS NOT NULL)
        FROM board_leases l
        JOIN tasks t ON t.id=$2
        JOIN runs r ON r.id=t.run_id
        JOIN board_sessions s ON s.lease_id=l.id AND s.board_id=l.board_id
        WHERE l.id=$1 AND l.board_id=$3 AND l.state='active'
          AND s.ended_at IS NULL AND s.owner_id=l.holder_id
          AND r.actor_id=l.holder_id
        FOR SHARE OF l,s,t,r`, leaseID, taskID, actor.boardID).
		Scan(&holderID, &scope, &taskState, &rawArguments, &cancelled)
	if err != nil {
		if err == pgx.ErrNoRows {
			return Attempt{}, fmt.Errorf("%w: no scheduled HIL task for active lease", ErrConflict)
		}
		return Attempt{}, fmt.Errorf("%w: read board HIL claim: %v", ErrUnavailable, err)
	}
	if scope != "hil" {
		return Attempt{}, fmt.Errorf("%w: board agent cannot claim a non-HIL task", ErrDenied)
	}
	if taskState == "running" {
		var existing Attempt
		err := tx.QueryRow(ctx, `SELECT a.id::text,a.task_id::text,a.attempt_no,a.state,a.started_at,a.deadline_at
            FROM task_attempts a
            WHERE a.task_id=$1 AND a.board_lease_id=$2 AND a.state='running'
              AND EXISTS (SELECT 1 FROM audit u WHERE u.actor_id=$3
                AND u.action='board.hil.attempt_claimed' AND u.target_type='attempt'
                AND u.target_id=a.id::text AND u.reason->>'lease_id'=$2)
            ORDER BY a.attempt_no DESC LIMIT 1`, taskID, leaseID, actor.id).
			Scan(&existing.ID, &existing.TaskID, &existing.AttemptNo, &existing.State, &existing.StartedAt, &existing.DeadlineAt)
		if err != nil {
			if err == pgx.ErrNoRows {
				return Attempt{}, fmt.Errorf("%w: HIL task already running under another claim", ErrConflict)
			}
			return Attempt{}, fmt.Errorf("%w: read existing HIL attempt: %v", ErrUnavailable, err)
		}
		if err := tx.Commit(ctx); err != nil {
			return Attempt{}, fmt.Errorf("%w: replay HIL claim: %v", ErrUnavailable, err)
		}
		return existing, nil
	}
	if taskState != "scheduled" {
		return Attempt{}, fmt.Errorf("%w: HIL task is %s", ErrConflict, taskState)
	}
	if cancelled {
		return Attempt{}, fmt.Errorf("%w: cancelled run cannot start another HIL task", ErrConflict)
	}
	var definition struct {
		Arguments []string         `json:"argv"`
		HIL       *catalog.HILTask `json:"hil"`
	}
	decoder := json.NewDecoder(bytes.NewReader(rawArguments))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&definition) != nil || definition.HIL == nil ||
		catalog.ValidateHILTaskMetadata(*definition.HIL) != nil || definition.HIL.BoardID != actor.boardID {
		return Attempt{}, fmt.Errorf("%w: HIL task does not match this board", ErrConflict)
	}
	if err := tx.Commit(ctx); err != nil {
		return Attempt{}, fmt.Errorf("%w: close HIL claim check: %v", ErrUnavailable, err)
	}

	facts.TaskID = taskID
	facts.ActorID = holderID
	facts.AgentID = ""
	facts.ClaimedBy = actor.id
	facts.BoardLeaseID = leaseID
	if facts.Engine == "" {
		facts.Engine = "board-agent"
	}
	if facts.Host == "" {
		facts.Host = "board-agent:" + actor.boardID
	}
	attempt, err := s.StartAttempt(ctx, facts)
	if err != nil {
		return Attempt{}, err
	}
	if attempt.TaskID != taskID {
		return Attempt{}, fmt.Errorf("%w: board HIL attempt task mismatch", ErrConflict)
	}
	return attempt, nil
}
