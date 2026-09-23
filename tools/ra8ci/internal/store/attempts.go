package store

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

// StartAttempt atomically claims one scheduled task whose dependencies have
// succeeded. Each claim creates a new attempt row rather than overwriting one.
func (s *Store) StartAttempt(ctx context.Context, in StartAttemptInput) (Attempt, error) {
	if in.AgentID != "" {
		return Attempt{}, fmt.Errorf("%w: remote agents require fenced ClaimAgentTask", ErrInvalid)
	}
	if !validHostFacts(in) {
		return Attempt{}, fmt.Errorf("%w: attempt host facts", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: begin attempt: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var runID string
	err = tx.QueryRow(ctx, "SELECT run_id::text FROM tasks WHERE id=$1", in.TaskID).Scan(&runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return Attempt{}, ErrNotFound
	}
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: find task: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return Attempt{}, fmt.Errorf("%w: lock run: %v", ErrUnavailable, err)
	}
	var state, scope string
	var taskArguments []byte
	var deadline int
	var version int64
	err = tx.QueryRow(ctx, `SELECT state,deadline_seconds,version,scope,arguments
		FROM tasks WHERE id=$1 FOR UPDATE`, in.TaskID).
		Scan(&state, &deadline, &version, &scope, &taskArguments)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: lock task: %v", ErrUnavailable, err)
	}
	if state != "scheduled" {
		return Attempt{}, fmt.Errorf("%w: task is %s", ErrConflict, state)
	}
	var boardLeaseID any
	if scope == "hil" {
		if !ValidID(in.BoardLeaseID) {
			return Attempt{}, fmt.Errorf("%w: HIL attempt requires an active board lease", ErrInvalid)
		}
		var definition struct {
			Arguments []string         `json:"argv"`
			HIL       *catalog.HILTask `json:"hil"`
		}
		decoder := json.NewDecoder(bytes.NewReader(taskArguments))
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&definition); err != nil || definition.HIL == nil ||
			catalog.ValidateHILTaskMetadata(*definition.HIL) != nil {
			return Attempt{}, fmt.Errorf("%w: persisted HIL task contract is invalid", ErrConflict)
		}
		if err := validateActiveHILLease(ctx, tx, in.BoardLeaseID, in.ActorID,
			definition.HIL.BoardID, deadline+definition.HIL.FlashRestoreSeconds); err != nil {
			return Attempt{}, err
		}
		boardLeaseID = in.BoardLeaseID
	} else if in.BoardLeaseID != "" {
		return Attempt{}, fmt.Errorf("%w: non-HIL attempt cannot claim a board lease", ErrInvalid)
	}
	var blocked int
	err = tx.QueryRow(ctx, `SELECT COUNT(*) FROM task_edges e
		JOIN tasks d ON d.id=e.depends_on_task_id WHERE e.task_id=$1 AND d.state<>'succeeded'`, in.TaskID).Scan(&blocked)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: check dependencies: %v", ErrUnavailable, err)
	}
	if blocked != 0 {
		return Attempt{}, fmt.Errorf("%w: dependencies have not succeeded", ErrConflict)
	}
	var attemptNo int
	err = tx.QueryRow(ctx, "SELECT COALESCE(MAX(attempt_no),0)+1 FROM task_attempts WHERE task_id=$1", in.TaskID).Scan(&attemptNo)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: next attempt number: %v", ErrUnavailable, err)
	}
	id, err := NewID()
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: %v", errEntropy, err)
	}
	facts, _ := validObject(in.HostFacts)
	var attempt Attempt
	var agent any
	if in.AgentID != "" {
		agent = in.AgentID
	}
	err = tx.QueryRow(ctx, `INSERT INTO task_attempts
		(id, task_id, attempt_no, agent_id, board_lease_id, state, engine, host, host_cores,
		host_ram_bytes, host_load, host_facts, started_at, deadline_at)
		VALUES ($1,$2,$3,$4,$5,'running',$6,$7,$8,$9,$10,$11,clock_timestamp(),
		clock_timestamp()+($12 * interval '1 second'))
		RETURNING id::text, task_id::text, attempt_no, state, started_at, deadline_at`,
		id, in.TaskID, attemptNo, agent, boardLeaseID, in.Engine, in.Host, in.HostCores,
		in.HostRAMBytes, in.HostLoad, facts, deadline).Scan(&attempt.ID, &attempt.TaskID,
		&attempt.AttemptNo, &attempt.State, &attempt.StartedAt, &attempt.DeadlineAt)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: insert attempt: %v", ErrUnavailable, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE tasks SET state='running', started_at=clock_timestamp(),
		version=version+1 WHERE id=$1 AND version=$2 AND state='scheduled'`, in.TaskID, version)
	if err != nil || tag.RowsAffected() != 1 {
		return Attempt{}, fmt.Errorf("%w: claim task: %v", ErrConflict, err)
	}
	var previousRunState string
	err = tx.QueryRow(ctx, "SELECT state FROM runs WHERE id=$1", runID).Scan(&previousRunState)
	if err != nil {
		return Attempt{}, fmt.Errorf("%w: read run state: %v", ErrUnavailable, err)
	}
	if previousRunState == "queued" {
		_, err = tx.Exec(ctx, `UPDATE runs SET state='running', started_at=clock_timestamp(), version=version+1 WHERE id=$1 AND state='queued'`, runID)
		if err != nil {
			return Attempt{}, fmt.Errorf("%w: start run: %v", ErrUnavailable, err)
		}
		if err := appendAudit(ctx, tx, in.ActorID, "run.started", "run", runID, "ok", "queued", "running", runID, nil); err != nil {
			return Attempt{}, fmt.Errorf("%w: audit run start: %v", ErrUnavailable, err)
		}
	}
	if err := appendAudit(ctx, tx, in.ActorID, "task.started", "task", in.TaskID, "ok", "scheduled", "running", runID, map[string]any{"attempt_id": id}); err != nil {
		return Attempt{}, fmt.Errorf("%w: audit task start: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "attempt.started", map[string]any{"task_id": in.TaskID, "attempt_id": id}); err != nil {
		return Attempt{}, fmt.Errorf("%w: event attempt start: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return Attempt{}, fmt.Errorf("%w: commit attempt start: %v", ErrUnavailable, err)
	}
	return attempt, nil
}

func validateActiveHILLease(ctx context.Context, tx pgx.Tx, leaseID, actorID, boardID string, requiredSeconds int) error {
	if requiredSeconds < 1 {
		return fmt.Errorf("%w: HIL lease budget", ErrInvalid)
	}
	var holderID, sessionBoardID, fixtureRevision, profileSHA string
	var enoughTime bool
	err := tx.QueryRow(ctx, `SELECT l.holder_id,s.board_id,s.fixture_revision,s.profile_sha256,
		l.expires_at > clock_timestamp()+($2 * interval '1 second')
		FROM board_leases l JOIN board_sessions s ON s.lease_id=l.id AND s.board_id=l.board_id
		WHERE l.id=$1 AND l.state='active' AND s.ended_at IS NULL
		AND s.owner_id=l.holder_id AND s.profile_sha256 IS NOT NULL
		FOR SHARE OF l,s`, leaseID, requiredSeconds).Scan(&holderID, &sessionBoardID,
		&fixtureRevision, &profileSHA, &enoughTime)
	if errors.Is(err, pgx.ErrNoRows) {
		return fmt.Errorf("%w: HIL board lease has no active fixture session", ErrConflict)
	}
	if err != nil {
		return fmt.Errorf("%w: verify HIL board lease: %v", ErrUnavailable, err)
	}
	if holderID != actorID || sessionBoardID != boardID || fixtureRevision == "" ||
		!hexSHA.MatchString(profileSHA) || !enoughTime {
		return fmt.Errorf("%w: HIL board lease identity or remaining duration is insufficient", ErrConflict)
	}
	return nil
}

// RecordStep persists a completed, named step exactly once with its measured
// monotonic duration. Duplicate step keys and ordinals fail transactionally.
func (s *Store) RecordStep(ctx context.Context, in StepInput) error {
	if !ValidID(in.AttemptID) || in.ActorID == "" || len(in.Key) == 0 || len(in.Key) > 128 || in.Ordinal < 0 || len(in.Phase) == 0 || len(in.Phase) > 64 || in.StartedAt.IsZero() || in.EndedAt.IsZero() || in.EndedAt.Before(in.StartedAt) || in.DurationNS < 0 || !validStepState(in.State) {
		return fmt.Errorf("%w: step metadata", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin step: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var taskID, runID string
	err = tx.QueryRow(ctx, `SELECT a.task_id::text, t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, in.AttemptID).Scan(&taskID, &runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: find attempt: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return fmt.Errorf("%w: lock run: %v", ErrUnavailable, err)
	}
	var state string
	err = tx.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1 FOR UPDATE", in.AttemptID).Scan(&state)
	if err != nil {
		return fmt.Errorf("%w: lock attempt: %v", ErrUnavailable, err)
	}
	if state != "running" {
		return fmt.Errorf("%w: attempt is %s", ErrConflict, state)
	}
	_, err = tx.Exec(ctx, `INSERT INTO task_steps
		(attempt_id, step_key, ordinal, phase, started_at, ended_at, duration_ns, state, child_exit_code)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)`, in.AttemptID, in.Key, in.Ordinal,
		in.Phase, in.StartedAt, in.EndedAt, in.DurationNS, in.State, in.ChildExitCode)
	if err != nil {
		return fmt.Errorf("%w: insert step: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, in.ActorID, "task.step", "task", taskID, "ok", "", in.State, runID, map[string]any{"attempt_id": in.AttemptID, "step_key": in.Key}); err != nil {
		return fmt.Errorf("%w: audit step: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "step.finished", map[string]any{"attempt_id": in.AttemptID, "step_key": in.Key, "state": in.State}); err != nil {
		return fmt.Errorf("%w: event step: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit step: %v", ErrUnavailable, err)
	}
	return nil
}

func validStepState(state string) bool {
	switch state {
	case "succeeded", "failed", "timed_out", "cancelled", "skipped":
		return true
	default:
		return false
	}
}

func validAttemptResult(in FinishAttemptInput) bool {
	if !ValidID(in.AttemptID) || in.ActorID == "" || len(in.Reason) > 1024 {
		return false
	}
	switch in.Result {
	case "succeeded":
		return in.ChildExitCode != nil && *in.ChildExitCode == 0 && !in.HitDeadline && in.EvidenceComplete
	case "failed":
		return !in.HitDeadline
	case "timed_out":
		return in.HitDeadline
	case "cancelled", "preempted", "lost":
		return !in.HitDeadline
	default:
		return false
	}
}

// FinishAttempt writes the exact result, skips every dependent scheduled task
// after non-success, and closes the run only when all its tasks are terminal.
func (s *Store) FinishAttempt(ctx context.Context, in FinishAttemptInput) error {
	if !validAttemptResult(in) {
		return fmt.Errorf("%w: terminal attempt result", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin finish: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var taskID, runID string
	err = tx.QueryRow(ctx, `SELECT a.task_id::text, t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, in.AttemptID).Scan(&taskID, &runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: find attempt: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return fmt.Errorf("%w: lock run: %v", ErrUnavailable, err)
	}
	var state string
	var version int64
	err = tx.QueryRow(ctx, "SELECT state, version FROM task_attempts WHERE id=$1 FOR UPDATE", in.AttemptID).Scan(&state, &version)
	if err != nil {
		return fmt.Errorf("%w: lock attempt: %v", ErrUnavailable, err)
	}
	if state != "running" {
		return fmt.Errorf("%w: attempt is %s", ErrConflict, state)
	}
	tag, err := tx.Exec(ctx, `UPDATE task_attempts SET state=$2, ended_at=clock_timestamp(),
		child_exit_code=$3, hit_deadline=$4, evidence_complete=$5, result_reason=$6,
		version=version+1 WHERE id=$1 AND version=$7 AND state='running'`,
		in.AttemptID, in.Result, in.ChildExitCode, in.HitDeadline, in.EvidenceComplete, nullable(in.Reason), version)
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: finish attempt: %v", ErrConflict, err)
	}
	var taskVersion int64
	err = tx.QueryRow(ctx, "SELECT version FROM tasks WHERE id=$1 AND state='running' FOR UPDATE", taskID).Scan(&taskVersion)
	if errors.Is(err, pgx.ErrNoRows) {
		return fmt.Errorf("%w: task no longer running", ErrConflict)
	}
	if err != nil {
		return fmt.Errorf("%w: lock task: %v", ErrUnavailable, err)
	}
	taskResult := in.Result
	if !in.EvidenceComplete && in.Result == "succeeded" {
		taskResult = "failed"
	}
	tag, err = tx.Exec(ctx, `UPDATE tasks SET state=$2, ended_at=clock_timestamp(), version=version+1
		WHERE id=$1 AND version=$3 AND state='running'`, taskID, taskResult, taskVersion)
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: finish task: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, in.ActorID, "task.attempt.finished", "task", taskID, "ok", "running", taskResult, runID, map[string]any{"attempt_id": in.AttemptID, "result": in.Result, "evidence_complete": in.EvidenceComplete}); err != nil {
		return fmt.Errorf("%w: audit task finish: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "attempt.finished", map[string]any{"task_id": taskID, "attempt_id": in.AttemptID, "result": in.Result, "evidence_complete": in.EvidenceComplete}); err != nil {
		return fmt.Errorf("%w: event attempt finish: %v", ErrUnavailable, err)
	}
	if taskResult != "succeeded" {
		if err := skipDescendants(ctx, tx, runID, taskID, in.ActorID); err != nil {
			return err
		}
	}
	if err := closeRunIfTerminal(ctx, tx, runID, in.ActorID); err != nil {
		return err
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit finish: %v", ErrUnavailable, err)
	}
	return nil
}

func skipDescendants(ctx context.Context, tx pgx.Tx, runID, failedTaskID, actor string) error {
	rows, err := tx.Query(ctx, `WITH RECURSIVE descendants(id) AS (
		SELECT task_id FROM task_edges WHERE run_id=$1 AND depends_on_task_id=$2
		UNION SELECT e.task_id FROM task_edges e JOIN descendants d ON e.depends_on_task_id=d.id WHERE e.run_id=$1
	) SELECT t.id::text FROM tasks t JOIN descendants d ON d.id=t.id WHERE t.state='scheduled' ORDER BY t.id`, runID, failedTaskID)
	if err != nil {
		return fmt.Errorf("%w: list dependent tasks: %v", ErrUnavailable, err)
	}
	var descendants []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return fmt.Errorf("%w: scan dependent task: %v", ErrUnavailable, err)
		}
		descendants = append(descendants, id)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return fmt.Errorf("%w: dependent tasks: %v", ErrUnavailable, err)
	}
	for _, id := range descendants {
		tag, err := tx.Exec(ctx, `UPDATE tasks SET state='skipped', skip_reason='prerequisite_failed',
			ended_at=clock_timestamp(), version=version+1 WHERE id=$1 AND state='scheduled'`, id)
		if err != nil {
			return fmt.Errorf("%w: skip dependent task: %v", ErrUnavailable, err)
		}
		if tag.RowsAffected() != 1 {
			continue
		}
		if err := appendAudit(ctx, tx, actor, "task.skipped", "task", id, "ok", "scheduled", "skipped", runID, map[string]any{"failed_prerequisite": failedTaskID}); err != nil {
			return fmt.Errorf("%w: audit skipped task: %v", ErrUnavailable, err)
		}
		if err := appendEvent(ctx, tx, runID, "task.skipped", map[string]any{"task_id": id, "reason": "prerequisite_failed"}); err != nil {
			return fmt.Errorf("%w: event skipped task: %v", ErrUnavailable, err)
		}
	}
	return nil
}

func closeRunIfTerminal(ctx context.Context, tx pgx.Tx, runID, actor string) error {
	var previousState string
	var cancelRequested bool
	if err := tx.QueryRow(ctx, `SELECT state, cancel_requested_at IS NOT NULL FROM runs WHERE id=$1`, runID).Scan(&previousState, &cancelRequested); err != nil {
		return fmt.Errorf("%w: read run state: %v", ErrUnavailable, err)
	}
	if previousState == "terminal" {
		return nil
	}
	var pending, unsuccessful, incomplete, timedOut int
	err := tx.QueryRow(ctx, `SELECT
		COUNT(*) FILTER (WHERE state IN ('scheduled','running')),
		COUNT(*) FILTER (WHERE state <> 'succeeded'),
		COUNT(*) FILTER (WHERE state = 'timed_out'),
		COUNT(*) FILTER (WHERE state = 'lost')
		FROM tasks WHERE run_id=$1`, runID).Scan(&pending, &unsuccessful, &timedOut, &incomplete)
	if err != nil {
		return fmt.Errorf("%w: summarize run: %v", ErrUnavailable, err)
	}
	if pending != 0 {
		return nil
	}
	var missingEvidence int
	err = tx.QueryRow(ctx, `SELECT COUNT(*) FROM task_attempts a JOIN tasks t ON t.id=a.task_id
		WHERE t.run_id=$1 AND a.state IN ('succeeded','failed','timed_out','cancelled','preempted','lost')
		AND NOT a.evidence_complete`, runID).Scan(&missingEvidence)
	if err != nil {
		return fmt.Errorf("%w: summarize evidence: %v", ErrUnavailable, err)
	}
	result := "succeeded"
	evidence := "complete"
	if missingEvidence != 0 {
		result = "incomplete_evidence"
		evidence = "incomplete"
	} else if cancelRequested {
		result = "cancelled"
	} else if timedOut != 0 {
		result = "timed_out"
	} else if unsuccessful != 0 {
		result = "failed"
	}
	_, err = tx.Exec(ctx, `UPDATE runs SET state='terminal', execution_result=$2,
		evidence_state=$3, ended_at=clock_timestamp(), version=version+1
		WHERE id=$1 AND state IN ('queued','running')`, runID, result, evidence)
	if err != nil {
		return fmt.Errorf("%w: finish run: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, "run.finished", "run", runID, "ok", previousState, "terminal", runID, map[string]any{"result": result, "evidence_state": evidence}); err != nil {
		return fmt.Errorf("%w: audit run finish: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "run.finished", map[string]any{"result": result, "evidence_state": evidence}); err != nil {
		return fmt.Errorf("%w: event run finish: %v", ErrUnavailable, err)
	}
	return nil
}
