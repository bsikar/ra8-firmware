package store

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

func validateHILSegmentAttempt(ctx context.Context, tx pgx.Tx, attemptID string, token board.Token, bound time.Duration) error {
	var raw []byte
	var enough bool
	err := tx.QueryRow(ctx, `SELECT t.arguments,
        a.deadline_at > clock_timestamp()+($4 * interval '1 millisecond')
        FROM task_attempts a JOIN tasks t ON t.id=a.task_id
        JOIN board_leases l ON l.id=a.board_lease_id
        JOIN board_sessions s ON s.lease_id=l.id AND s.board_id=l.board_id
        WHERE a.id=$1 AND a.board_lease_id=$2 AND a.state='running'
          AND t.state='running' AND t.scope='hil'
          AND l.board_id=$3 AND l.generation=$5 AND l.state='active'
          AND s.ended_at IS NULL AND s.owner_id=l.holder_id
          AND s.fixture_revision<>'' AND s.profile_sha256 ~ '^[a-f0-9]{64}$'
        FOR SHARE OF a,t,l,s`, attemptID, token.LeaseID, token.BoardID, bound.Milliseconds(), int64(token.Generation)).Scan(&raw, &enough)
	if err != nil {
		if err == pgx.ErrNoRows {
			return fmt.Errorf("%w: HIL attempt is not active under this board lease", ErrConflict)
		}
		return fmt.Errorf("%w: validate HIL attempt: %v", ErrUnavailable, err)
	}
	if !enough {
		return fmt.Errorf("%w: HIL attempt deadline is too close", ErrConflict)
	}
	var args struct {
		HIL *catalog.HILTask `json:"hil"`
	}
	if err := json.Unmarshal(raw, &args); err != nil || args.HIL == nil ||
		catalog.ValidateHILTaskMetadata(*args.HIL) != nil || args.HIL.BoardID != token.BoardID {
		return fmt.Errorf("%w: HIL contract does not match board", ErrConflict)
	}
	return nil
}
