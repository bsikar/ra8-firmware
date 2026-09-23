package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/jackc/pgx/v5"
)

type GitHubMessage struct {
	ScaleSetID string          `json:"scale_set_id"`
	SessionID  string          `json:"session_id"`
	MessageID  string          `json:"message_id"`
	Payload    json.RawMessage `json:"payload"`
}

func validMessage(in GitHubMessage) bool {
	if len(in.ScaleSetID) < 1 || len(in.ScaleSetID) > 128 || len(in.SessionID) < 1 || len(in.SessionID) > 256 || len(in.MessageID) < 1 || len(in.MessageID) > 256 {
		return false
	}
	_, ok := validObject(in.Payload)
	return ok
}

// SaveGitHubMessage is the durable-before-ACK barrier for scale-set messages.
// The same identity and payload may be replayed, but conflicting reuse fails.
func (s *Store) SaveGitHubMessage(ctx context.Context, scaleSetID, sessionID, messageID string, payload json.RawMessage) error {
	in := GitHubMessage{ScaleSetID: scaleSetID, SessionID: sessionID, MessageID: messageID, Payload: payload}
	if !validMessage(in) {
		return fmt.Errorf("%w: GitHub message", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin inbox insert: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var inserted string
	err = tx.QueryRow(ctx, `INSERT INTO github_scaleset_inbox
		(scale_set_id, session_id, message_id, payload) VALUES ($1,$2,$3,$4)
		ON CONFLICT DO NOTHING RETURNING message_id`, scaleSetID, sessionID, messageID, payload).Scan(&inserted)
	if errors.Is(err, pgx.ErrNoRows) {
		var same bool
		err = tx.QueryRow(ctx, `SELECT payload=$4::jsonb FROM github_scaleset_inbox
			WHERE scale_set_id=$1 AND session_id=$2 AND message_id=$3`, scaleSetID, sessionID, messageID, payload).Scan(&same)
		if err != nil {
			return fmt.Errorf("%w: replay lookup: %v", ErrUnavailable, err)
		}
		if !same {
			return fmt.Errorf("%w: GitHub message ID reused with different payload", ErrConflict)
		}
	} else if err != nil {
		return fmt.Errorf("%w: inbox insert: %v", ErrUnavailable, err)
	} else {
		if err := appendAudit(ctx, tx, "github-scaleset", "github.message.received", "github_message", scaleSetID+"/"+sessionID+"/"+messageID, "ok", "", "pending", "", nil); err != nil {
			return fmt.Errorf("%w: inbox audit: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit inbox: %v", ErrUnavailable, err)
	}
	return nil
}

func (s *Store) MarkGitHubMessageProcessed(ctx context.Context, scaleSetID, sessionID, messageID string) error {
	if !validMessage(GitHubMessage{ScaleSetID: scaleSetID, SessionID: sessionID, MessageID: messageID, Payload: json.RawMessage(`{}`)}) {
		return fmt.Errorf("%w: GitHub message identity", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin inbox update: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var wasPending bool
	err = tx.QueryRow(ctx, `SELECT processed_at IS NULL FROM github_scaleset_inbox
		WHERE scale_set_id=$1 AND session_id=$2 AND message_id=$3 FOR UPDATE`, scaleSetID, sessionID, messageID).Scan(&wasPending)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: find inbox message: %v", ErrUnavailable, err)
	}
	if wasPending {
		_, err = tx.Exec(ctx, `UPDATE github_scaleset_inbox SET processed_at=clock_timestamp()
			WHERE scale_set_id=$1 AND session_id=$2 AND message_id=$3 AND processed_at IS NULL`, scaleSetID, sessionID, messageID)
		if err != nil {
			return fmt.Errorf("%w: mark inbox processed: %v", ErrUnavailable, err)
		}
		if err := appendAudit(ctx, tx, "github-scaleset", "github.message.processed", "github_message", scaleSetID+"/"+sessionID+"/"+messageID, "ok", "pending", "processed", "", nil); err != nil {
			return fmt.Errorf("%w: inbox processed audit: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit inbox update: %v", ErrUnavailable, err)
	}
	return nil
}

func (s *Store) ListPendingGitHubMessages(ctx context.Context, limit int) ([]GitHubMessage, error) {
	if limit < 1 || limit > 1000 {
		return nil, fmt.Errorf("%w: inbox page size", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT scale_set_id, session_id, message_id, payload
		FROM github_scaleset_inbox WHERE processed_at IS NULL ORDER BY received_at LIMIT $1`, limit)
	if err != nil {
		return nil, fmt.Errorf("%w: inbox query: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	messages := make([]GitHubMessage, 0)
	for rows.Next() {
		var msg GitHubMessage
		if err := rows.Scan(&msg.ScaleSetID, &msg.SessionID, &msg.MessageID, &msg.Payload); err != nil {
			return nil, fmt.Errorf("%w: inbox scan: %v", ErrUnavailable, err)
		}
		messages = append(messages, msg)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: inbox rows: %v", ErrUnavailable, err)
	}
	return messages, nil
}

// ListPendingGitHubMessagesForScaleSet prevents another scale set's backlog
// from starving replay for the requested controller session.
func (s *Store) ListPendingGitHubMessagesForScaleSet(ctx context.Context, scaleSetID string, limit int) ([]GitHubMessage, error) {
	if len(scaleSetID) < 1 || len(scaleSetID) > 128 || limit < 1 || limit > 1000 {
		return nil, fmt.Errorf("%w: scale-set inbox page", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT scale_set_id, session_id, message_id, payload
		FROM github_scaleset_inbox WHERE scale_set_id=$1 AND processed_at IS NULL
		ORDER BY received_at, session_id, message_id LIMIT $2`, scaleSetID, limit)
	if err != nil {
		return nil, fmt.Errorf("%w: scale-set inbox query: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	messages := make([]GitHubMessage, 0)
	for rows.Next() {
		var msg GitHubMessage
		if err := rows.Scan(&msg.ScaleSetID, &msg.SessionID, &msg.MessageID, &msg.Payload); err != nil {
			return nil, fmt.Errorf("%w: scale-set inbox scan: %v", ErrUnavailable, err)
		}
		messages = append(messages, msg)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: scale-set inbox rows: %v", ErrUnavailable, err)
	}
	return messages, nil
}
