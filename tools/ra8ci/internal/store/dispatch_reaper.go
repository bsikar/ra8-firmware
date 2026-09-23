package store

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

type expiredAssignment struct {
	AttemptID string
	RunID     string
}

// ReapAgentAssignments fences attempts that never supplied a terminal receipt.
// The evidence grace includes child shutdown and post-deadline log delivery.
// A run cannot remain blocked forever by a crashed or revoked agent.
func (s *Store) ReapAgentAssignments(ctx context.Context, definitions *catalog.Catalog, limit int) (int, error) {
	if s == nil || s.pool == nil || definitions == nil || limit < 1 || limit > 1000 {
		return 0, fmt.Errorf("%w: reaper configuration", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT a.id::text, t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE a.assignment_id IS NOT NULL
		AND a.state IN ('issued','acknowledged','running')
		AND a.deadline_at < clock_timestamp()-($1 * interval '1 second')
		ORDER BY a.deadline_at, a.id LIMIT $2`, int(agentReaperGrace.Seconds()), limit)
	if err != nil {
		return 0, fmt.Errorf("%w: select expired assignments: %v", ErrUnavailable, err)
	}
	var candidates []expiredAssignment
	for rows.Next() {
		var candidate expiredAssignment
		if err := rows.Scan(&candidate.AttemptID, &candidate.RunID); err != nil {
			rows.Close()
			return 0, fmt.Errorf("%w: scan expired assignment: %v", ErrUnavailable, err)
		}
		candidates = append(candidates, candidate)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return 0, fmt.Errorf("%w: expired assignment rows: %v", ErrUnavailable, err)
	}
	rows.Close()
	reaped := 0
	for _, candidate := range candidates {
		changed, err := s.reapOneAgentAssignment(ctx, definitions, candidate)
		if err != nil {
			return reaped, err
		}
		if changed {
			reaped++
		}
	}
	return reaped, nil
}

func (s *Store) reapOneAgentAssignment(ctx context.Context, definitions *catalog.Catalog, candidate expiredAssignment) (bool, error) {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return false, fmt.Errorf("%w: begin assignment reaper: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := withRunLock(ctx, tx, candidate.RunID); err != nil {
		return false, fmt.Errorf("%w: lock expired run: %v", ErrUnavailable, err)
	}
	var taskID, taskName, state, catalogSHA string
	var attemptNo int
	var deadline time.Time
	err = tx.QueryRow(ctx, `SELECT a.task_id::text, t.name, a.state,
		r.catalog_sha256, a.attempt_no, a.deadline_at FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id JOIN runs r ON r.id=t.run_id
		WHERE a.id=$1 AND t.run_id=$2 FOR UPDATE OF a`,
		candidate.AttemptID, candidate.RunID).Scan(&taskID, &taskName, &state,
		&catalogSHA, &attemptNo, &deadline)
	if errors.Is(err, pgx.ErrNoRows) {
		return false, ErrNotFound
	}
	if err != nil {
		return false, fmt.Errorf("%w: lock expired attempt: %v", ErrUnavailable, err)
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
		return false, fmt.Errorf("%w: reaper clock: %v", ErrUnavailable, err)
	}
	if state != "issued" && state != "acknowledged" && state != "running" {
		return false, nil
	}
	if !databaseNow.After(deadline.Add(agentReaperGrace)) {
		return false, nil
	}
	_, err = tx.Exec(ctx, `UPDATE task_attempts SET state='lost',
		ended_at=clock_timestamp(), evidence_complete=false,
		result_reason='assignment_deadline_without_terminal', version=version+1
		WHERE id=$1`, candidate.AttemptID)
	if err != nil {
		return false, fmt.Errorf("%w: fence expired attempt: %v", ErrUnavailable, err)
	}
	definition, found := definitions.Task(taskName)
	retry := found && catalogSHA == definitions.Digest() && attemptNo < definition.Retry.MaxAttempts
	if retry {
		_, err = tx.Exec(ctx, `UPDATE tasks SET state='scheduled',
			started_at=NULL, ended_at=NULL, version=version+1
			WHERE id=$1 AND state='running'`, taskID)
	} else {
		_, err = tx.Exec(ctx, `UPDATE tasks SET state='lost',
			ended_at=clock_timestamp(), version=version+1
			WHERE id=$1 AND state='running'`, taskID)
	}
	if err != nil {
		return false, fmt.Errorf("%w: advance expired task: %v", ErrUnavailable, err)
	}
	next := "lost"
	if retry {
		next = "scheduled"
	}
	if err := appendAudit(ctx, tx, "ra8ci-server", "task.assignment.killed", "task",
		taskID, "ok", "running", next, candidate.RunID,
		map[string]any{"attempt_id": candidate.AttemptID, "reason": "assignment_deadline_without_terminal", "retry": retry}); err != nil {
		return false, fmt.Errorf("%w: audit assignment reap: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, candidate.RunID, "assignment.reaped", map[string]any{
		"task_id": taskID, "attempt_id": candidate.AttemptID, "retry": retry}); err != nil {
		return false, fmt.Errorf("%w: event assignment reap: %v", ErrUnavailable, err)
	}
	if !retry {
		if err := skipDescendants(ctx, tx, candidate.RunID, taskID, "ra8ci-server"); err != nil {
			return false, err
		}
		if err := closeRunIfTerminal(ctx, tx, candidate.RunID, "ra8ci-server"); err != nil {
			return false, err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return false, fmt.Errorf("%w: commit assignment reap: %v", ErrUnavailable, err)
	}
	return true, nil
}
