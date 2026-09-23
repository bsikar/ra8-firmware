// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"fmt"
)

// RequestRunCancellation records durable cooperative cancellation intent.
// Scheduled tasks are terminalized immediately; assigned tasks stop at their
// next heartbeat and retain their ordinary fenced terminal-receipt path.
func (s *Store) RequestRunCancellation(ctx context.Context, runID, actor string) (Run, error) {
	if !ValidID(runID) || actor == "" || len(actor) > 256 {
		return Run{}, fmt.Errorf("%w: run cancellation identity", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Run{}, fmt.Errorf("%w: begin run cancellation: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := withRunLock(ctx, tx, runID); err != nil {
		return Run{}, err
	}
	var state string
	var alreadyRequested bool
	if err := tx.QueryRow(ctx, `SELECT state, cancel_requested_at IS NOT NULL
		FROM runs WHERE id=$1`, runID).Scan(&state, &alreadyRequested); err != nil {
		return Run{}, fmt.Errorf("%w: read run cancellation state: %v", ErrUnavailable, err)
	}
	if state == "terminal" || alreadyRequested {
		if err := tx.Rollback(ctx); err != nil {
			return Run{}, fmt.Errorf("%w: finish idempotent cancellation: %v", ErrUnavailable, err)
		}
		return s.GetRun(ctx, runID)
	}
	if _, err := tx.Exec(ctx, `UPDATE runs SET cancel_requested_at=clock_timestamp(),
		cancel_requested_by=$2, version=version+1 WHERE id=$1`, runID, actor); err != nil {
		return Run{}, fmt.Errorf("%w: persist cancellation intent: %v", ErrUnavailable, err)
	}
	rows, err := tx.Query(ctx, `UPDATE tasks SET state='cancelled', ended_at=clock_timestamp(),
		version=version+1 WHERE run_id=$1 AND state='scheduled' RETURNING id::text, task_key`, runID)
	if err != nil {
		return Run{}, fmt.Errorf("%w: cancel scheduled tasks: %v", ErrUnavailable, err)
	}
	var cancelled []struct{ id, key string }
	for rows.Next() {
		var task struct{ id, key string }
		if err := rows.Scan(&task.id, &task.key); err != nil {
			rows.Close()
			return Run{}, fmt.Errorf("%w: read cancelled tasks: %v", ErrUnavailable, err)
		}
		cancelled = append(cancelled, task)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return Run{}, fmt.Errorf("%w: read cancelled tasks: %v", ErrUnavailable, err)
	}
	rows.Close()
	for _, task := range cancelled {
		if err := appendAudit(ctx, tx, actor, "task.cancelled_before_assignment", "task", task.id, "ok", "scheduled", "cancelled", runID, map[string]any{"key": task.key}); err != nil {
			return Run{}, fmt.Errorf("%w: audit cancelled task: %v", ErrUnavailable, err)
		}
		if err := appendEvent(ctx, tx, runID, "task.cancelled_before_assignment", map[string]any{"task_id": task.id, "key": task.key}); err != nil {
			return Run{}, fmt.Errorf("%w: event cancelled task: %v", ErrUnavailable, err)
		}
	}
	if err := appendAudit(ctx, tx, actor, "run.cancel.requested", "run", runID, "ok", state, state, runID, map[string]any{"scheduled_tasks_cancelled": len(cancelled)}); err != nil {
		return Run{}, fmt.Errorf("%w: audit run cancellation: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "run.cancel_requested", map[string]any{"actor": actor, "scheduled_tasks_cancelled": len(cancelled)}); err != nil {
		return Run{}, fmt.Errorf("%w: event run cancellation: %v", ErrUnavailable, err)
	}
	if err := closeRunIfTerminal(ctx, tx, runID, actor); err != nil {
		return Run{}, err
	}
	if err := tx.Commit(ctx); err != nil {
		return Run{}, fmt.Errorf("%w: commit run cancellation: %v", ErrUnavailable, err)
	}
	return s.GetRun(ctx, runID)
}
