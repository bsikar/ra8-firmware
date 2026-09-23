package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5"
)

// CreateRun validates the complete task DAG, then inserts the run, tasks,
// edges, audit history, event, and idempotency result in one transaction.
func (s *Store) CreateRun(ctx context.Context, in CreateRunInput) (Run, error) {
	if err := validateRun(in); err != nil {
		return Run{}, err
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Run{}, fmt.Errorf("%w: begin run: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if in.IdempotencyKey != "" {
		lockKey := fmt.Sprintf("%d:%s%s", len(in.ActorID), in.ActorID, in.IdempotencyKey)
		if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))", lockKey); err != nil {
			return Run{}, fmt.Errorf("%w: idempotency lock: %v", ErrUnavailable, err)
		}
		var oldHash, oldID string
		err := tx.QueryRow(ctx, `SELECT request_sha256, response_body->>'id' FROM idempotency_keys
			WHERE principal_id=$1 AND method='POST' AND path='/v1/runs' AND key=$2`, in.ActorID, in.IdempotencyKey).Scan(&oldHash, &oldID)
		if err == nil {
			if oldHash != in.RequestSHA256 {
				return Run{}, fmt.Errorf("%w: idempotency key reused for different request", ErrConflict)
			}
			if err := tx.Rollback(ctx); err != nil {
				return Run{}, fmt.Errorf("%w: close idempotent transaction: %v", ErrUnavailable, err)
			}
			return s.GetRun(ctx, oldID)
		}
		if !errors.Is(err, pgx.ErrNoRows) {
			return Run{}, fmt.Errorf("%w: idempotency lookup: %v", ErrUnavailable, err)
		}
	}
	if in.ParentRunID != "" {
		var parentRepo string
		err := tx.QueryRow(ctx, "SELECT repository FROM runs WHERE id=$1", in.ParentRunID).Scan(&parentRepo)
		if errors.Is(err, pgx.ErrNoRows) {
			return Run{}, fmt.Errorf("%w: parent run", ErrNotFound)
		}
		if err != nil {
			return Run{}, fmt.Errorf("%w: parent lookup: %v", ErrUnavailable, err)
		}
		if parentRepo != in.Repository {
			return Run{}, fmt.Errorf("%w: parent repository mismatch", ErrInvalid)
		}
	}
	runID, err := NewID()
	if err != nil {
		return Run{}, fmt.Errorf("%w: %v", errEntropy, err)
	}
	_, err = tx.Exec(ctx, `INSERT INTO runs
		(id, trigger, actor_id, repository, branch, commit_sha, snapshot_sha256, catalog_sha256, parent_run_id, state)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,'queued')`, runID, in.Trigger, in.ActorID, in.Repository, in.Branch, in.CommitSHA, in.SnapshotSHA256, in.CatalogSHA256, nullable(in.ParentRunID))
	if err != nil {
		return Run{}, fmt.Errorf("%w: insert run: %v", ErrUnavailable, err)
	}
	ids := make(map[string]string, len(in.Tasks))
	for _, task := range in.Tasks {
		id, idErr := NewID()
		if idErr != nil {
			return Run{}, fmt.Errorf("%w: %v", errEntropy, idErr)
		}
		ids[task.Key] = id
		args, _ := validObject(task.Arguments)
		_, err = tx.Exec(ctx, `INSERT INTO tasks
			(id, run_id, task_key, name, arguments, tier, scope, host_class, state, deadline_seconds)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,'scheduled',$9)`, id, runID, task.Key, task.Name, args, task.Tier, task.Scope, task.HostClass, task.DeadlineSeconds)
		if err != nil {
			return Run{}, fmt.Errorf("%w: insert task %q: %v", ErrUnavailable, task.Key, err)
		}
		if err := appendAudit(ctx, tx, in.ActorID, "task.scheduled", "task", id, "ok", "", "scheduled", runID, map[string]any{"key": task.Key}); err != nil {
			return Run{}, fmt.Errorf("%w: audit scheduled task: %v", ErrUnavailable, err)
		}
	}
	for _, task := range in.Tasks {
		for _, dependency := range task.DependsOnKeys {
			_, err = tx.Exec(ctx, "INSERT INTO task_edges (run_id, task_id, depends_on_task_id) VALUES ($1,$2,$3)", runID, ids[task.Key], ids[dependency])
			if err != nil {
				return Run{}, fmt.Errorf("%w: insert dependency: %v", ErrUnavailable, err)
			}
		}
	}
	if err := appendAudit(ctx, tx, in.ActorID, "run.created", "run", runID, "ok", "", "queued", runID, map[string]any{"tasks": len(in.Tasks)}); err != nil {
		return Run{}, fmt.Errorf("%w: audit run: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "run.created", map[string]any{"task_count": len(in.Tasks)}); err != nil {
		return Run{}, fmt.Errorf("%w: event run: %v", ErrUnavailable, err)
	}
	if in.IdempotencyKey != "" {
		_, err = tx.Exec(ctx, `INSERT INTO idempotency_keys
			(principal_id, method, path, key, request_sha256, response_status, response_body, expires_at)
			VALUES ($1,'POST','/v1/runs',$2,$3,201,jsonb_build_object('id',$4::text,'state','queued'),clock_timestamp()+interval '7 days')`,
			in.ActorID, in.IdempotencyKey, in.RequestSHA256, runID)
		if err != nil {
			return Run{}, fmt.Errorf("%w: persist idempotency: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return Run{}, fmt.Errorf("%w: commit run: %v", ErrUnavailable, err)
	}
	return s.GetRun(ctx, runID)
}

func validateRun(in CreateRunInput) error {
	if len(in.Trigger) < 1 || len(in.Trigger) > 64 || len(in.ActorID) < 1 || len(in.ActorID) > 256 || len(in.Repository) < 1 || len(in.Repository) > 512 || len(in.Branch) > 512 || !commitSHA.MatchString(in.CommitSHA) || !hexSHA.MatchString(in.SnapshotSHA256) || !hexSHA.MatchString(in.CatalogSHA256) || (in.ParentRunID != "" && !ValidID(in.ParentRunID)) {
		return fmt.Errorf("%w: run metadata", ErrInvalid)
	}
	if (in.IdempotencyKey == "") != (in.RequestSHA256 == "") || len(in.IdempotencyKey) > 256 || (in.IdempotencyKey != "" && !hexSHA.MatchString(in.RequestSHA256)) {
		return fmt.Errorf("%w: idempotency fields", ErrInvalid)
	}
	if len(in.Tasks) == 0 || len(in.Tasks) > 100 {
		return fmt.Errorf("%w: task count must be 1..100", ErrInvalid)
	}
	tasks := make(map[string]TaskInput, len(in.Tasks))
	for _, task := range in.Tasks {
		if len(task.Key) < 1 || len(task.Key) > 128 || len(task.Name) < 1 || len(task.Name) > 128 || strings.TrimSpace(task.Key) != task.Key || strings.TrimSpace(task.Name) != task.Name || (task.Tier != "required" && task.Tier != "optional" && task.Tier != "nightly") || !validScope(task.Scope) || task.DeadlineSeconds < 1 || task.DeadlineSeconds > 86400 || len(task.HostClass) > 128 || len(task.DependsOnKeys) > 100 {
			return fmt.Errorf("%w: invalid task %q", ErrInvalid, task.Key)
		}
		if _, exists := tasks[task.Key]; exists {
			return fmt.Errorf("%w: duplicate task key %q", ErrInvalid, task.Key)
		}
		if _, ok := validObject(task.Arguments); !ok {
			return fmt.Errorf("%w: task %q arguments must be an object", ErrInvalid, task.Key)
		}
		tasks[task.Key] = task
	}
	state := make(map[string]uint8, len(tasks))
	var visit func(string) error
	visit = func(key string) error {
		if state[key] == 1 {
			return fmt.Errorf("%w: task dependency cycle at %q", ErrInvalid, key)
		}
		if state[key] == 2 {
			return nil
		}
		state[key] = 1
		seen := make(map[string]bool)
		for _, dependency := range tasks[key].DependsOnKeys {
			if _, exists := tasks[dependency]; !exists || seen[dependency] {
				return fmt.Errorf("%w: missing or repeated dependency %q for %q", ErrInvalid, dependency, key)
			}
			seen[dependency] = true
			if err := visit(dependency); err != nil {
				return err
			}
		}
		state[key] = 2
		return nil
	}
	for key := range tasks {
		if err := visit(key); err != nil {
			return err
		}
	}
	return nil
}

func validScope(scope string) bool {
	switch scope {
	case "safe-local-read-only", "safe-local-write-working-tree", "runner", "linux-vm", "windows-vm", "hil":
		return true
	default:
		return false
	}
}

// GetRun returns durable status, never an inferred or optimistic result.
func (s *Store) GetRun(ctx context.Context, id string) (Run, error) {
	if !ValidID(id) {
		return Run{}, fmt.Errorf("%w: run ID", ErrInvalid)
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.RepeatableRead, AccessMode: pgx.ReadOnly})
	if err != nil {
		return Run{}, fmt.Errorf("%w: begin run read: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var run Run
	var parent, result, cancelActor sql.NullString
	err = tx.QueryRow(ctx, `SELECT id::text, trigger, actor_id, repository, branch, commit_sha,
		snapshot_sha256, catalog_sha256, parent_run_id::text, state, execution_result,
		cleanup_result, evidence_state, created_at, started_at, ended_at, version,
		cancel_requested_at, cancel_requested_by
		FROM runs WHERE id=$1`, id).Scan(&run.ID, &run.Trigger, &run.ActorID, &run.Repository, &run.Branch,
		&run.CommitSHA, &run.SnapshotSHA256, &run.CatalogSHA256, &parent, &run.State, &result,
		&run.CleanupResult, &run.EvidenceState, &run.CreatedAt, &run.StartedAt, &run.EndedAt, &run.Version,
		&run.CancelRequestedAt, &cancelActor)
	if errors.Is(err, pgx.ErrNoRows) {
		return Run{}, ErrNotFound
	}
	if err != nil {
		return Run{}, fmt.Errorf("%w: read run: %v", ErrUnavailable, err)
	}
	run.ParentRunID = parent.String
	run.CancelRequestedBy = cancelActor.String
	run.ExecutionResult = result.String
	rows, err := tx.Query(ctx, `SELECT id::text, run_id::text, task_key, name, arguments, tier, scope,
		host_class, state, skip_reason, deadline_seconds, enqueued_at, started_at, ended_at, version
		FROM tasks WHERE run_id=$1 ORDER BY enqueued_at, task_key`, id)
	if err != nil {
		return Run{}, fmt.Errorf("%w: read tasks: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	run.Tasks = make([]Task, 0)
	for rows.Next() {
		var task Task
		var skip sql.NullString
		if err := rows.Scan(&task.ID, &task.RunID, &task.Key, &task.Name, &task.Arguments, &task.Tier, &task.Scope,
			&task.HostClass, &task.State, &skip, &task.DeadlineSeconds, &task.EnqueuedAt,
			&task.StartedAt, &task.EndedAt, &task.Version); err != nil {
			return Run{}, fmt.Errorf("%w: scan task: %v", ErrUnavailable, err)
		}
		task.SkipReason = skip.String
		task.AttemptIDs = []string{}
		run.Tasks = append(run.Tasks, task)
	}
	if err := rows.Err(); err != nil {
		return Run{}, fmt.Errorf("%w: task rows: %v", ErrUnavailable, err)
	}
	rows.Close()
	attemptRows, err := tx.Query(ctx, `SELECT a.task_id::text, a.id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE t.run_id=$1 ORDER BY a.task_id, a.attempt_no`, id)
	if err != nil {
		return Run{}, fmt.Errorf("%w: read attempt IDs: %v", ErrUnavailable, err)
	}
	defer attemptRows.Close()
	index := make(map[string]int, len(run.Tasks))
	for i := range run.Tasks {
		index[run.Tasks[i].ID] = i
	}
	for attemptRows.Next() {
		var taskID, attemptID string
		if err := attemptRows.Scan(&taskID, &attemptID); err != nil {
			return Run{}, fmt.Errorf("%w: scan attempt ID: %v", ErrUnavailable, err)
		}
		i, exists := index[taskID]
		if !exists {
			return Run{}, fmt.Errorf("%w: orphan attempt %s", ErrUnavailable, attemptID)
		}
		run.Tasks[i].AttemptIDs = append(run.Tasks[i].AttemptIDs, attemptID)
	}
	if err := attemptRows.Err(); err != nil {
		return Run{}, fmt.Errorf("%w: attempt rows: %v", ErrUnavailable, err)
	}
	attemptRows.Close()
	if err := tx.Commit(ctx); err != nil {
		return Run{}, fmt.Errorf("%w: commit run read: %v", ErrUnavailable, err)
	}
	return run, nil
}

// GetRunEvents returns an ordered page. It is intentionally bounded.
func (s *Store) GetRunEvents(ctx context.Context, id string, after int64, limit int) ([]json.RawMessage, error) {
	if !ValidID(id) || after < 0 || limit < 1 || limit > 1000 {
		return nil, fmt.Errorf("%w: event page", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT jsonb_build_object('seq', event_seq, 'kind', kind,
		'data', data, 'happened_at', happened_at) FROM run_events
		WHERE run_id=$1 AND event_seq>$2 ORDER BY event_seq LIMIT $3`, id, after, limit)
	if err != nil {
		return nil, fmt.Errorf("%w: event page: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	items := make([]json.RawMessage, 0)
	for rows.Next() {
		var data json.RawMessage
		if err := rows.Scan(&data); err != nil {
			return nil, fmt.Errorf("%w: event scan: %v", ErrUnavailable, err)
		}
		items = append(items, data)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: event rows: %v", ErrUnavailable, err)
	}
	return items, nil
}

func withRunLock(ctx context.Context, tx pgx.Tx, runID string) error {
	var found string
	err := tx.QueryRow(ctx, "SELECT id::text FROM runs WHERE id=$1 FOR UPDATE", runID).Scan(&found)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	return err
}
