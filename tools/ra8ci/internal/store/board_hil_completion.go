// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

// BoardHILCompletion is the board agent's terminal evidence for one claimed
// HIL attempt. Task identity and step names are checked against the catalog.
type BoardHILCompletion struct {
	AttemptID        string    `json:"attempt_id"`
	LeaseID          string    `json:"lease_id"`
	Generation       uint64    `json:"generation"`
	Result           string    `json:"result"`
	ChildExitCode    *int      `json:"child_exit_code,omitempty"`
	HitDeadline      bool      `json:"hit_deadline"`
	EvidenceComplete bool      `json:"evidence_complete"`
	Reason           string    `json:"reason,omitempty"`
	Steps            []HILStep `json:"steps"`
}

// HILStep contains measured timing and terminal status, never command text.
type HILStep struct {
	Key           string    `json:"key"`
	StartedAt     time.Time `json:"started_at"`
	EndedAt       time.Time `json:"ended_at"`
	DurationNS    int64     `json:"duration_ns"`
	State         string    `json:"state"`
	ChildExitCode *int      `json:"child_exit_code,omitempty"`
}

// HILDefinitionCatalog is the immutable catalog view required to verify a
// server-assigned HIL task at completion time.
type HILDefinitionCatalog interface {
	Digest() string
	Task(string) (catalog.Task, bool)
}

// CompleteBoardHILAttempt commits step evidence and the terminal task result
// together. It is bound to the board-agent certificate, lease generation,
// holder, run actor, and immutable HIL catalog entry.
func (s *Store) CompleteBoardHILAttempt(ctx context.Context, actor BoardActor,
	in BoardHILCompletion, definitions HILDefinitionCatalog, trustedCommit string) error {
	if s == nil || s.pool == nil || ctx == nil || actor.kind != "board_agent" ||
		actor.role != "board_agent" || !validBoardID(actor.boardID) ||
		!ValidID(in.AttemptID) || !ValidID(in.LeaseID) || in.Generation == 0 ||
		definitions == nil || definitions.Digest() == "" || !commitSHA.MatchString(trustedCommit) ||
		len(in.Steps) == 0 || len(in.Steps) > 64 {
		return fmt.Errorf("%w: board HIL completion arguments", ErrInvalid)
	}
	finish := FinishAttemptInput{AttemptID: in.AttemptID, ActorID: actor.id, Result: in.Result,
		ChildExitCode: in.ChildExitCode, HitDeadline: in.HitDeadline,
		EvidenceComplete: in.EvidenceComplete, Reason: in.Reason}
	if !validAttemptResult(finish) || (in.ChildExitCode != nil && (*in.ChildExitCode < 0 || *in.ChildExitCode > 255)) {
		return fmt.Errorf("%w: board HIL terminal result", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin board HIL completion: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return fmt.Errorf("%w: board HIL completion lock: %v", ErrUnavailable, err)
	}
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return err
	}
	var runID string
	err = tx.QueryRow(ctx, `SELECT t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE a.id=$1`, in.AttemptID).Scan(&runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: locate board HIL run: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return fmt.Errorf("%w: lock HIL completion run: %v", ErrUnavailable, err)
	}
	var taskID, selectedRunID, runActor, taskName, scope, attemptState, leaseID, holderID, leaseState string
	var taskDeadline int
	var runCancelled bool
	var rawArguments []byte
	var runCatalog, runCommit string
	var attemptStarted, attemptDeadline time.Time
	var generation int64
	var storedExit sql.NullInt32
	var storedTimedOut, storedEvidence bool
	var storedReason sql.NullString
	err = tx.QueryRow(ctx, `SELECT t.id::text,r.id::text,r.actor_id,t.name,t.scope,
	t.deadline_seconds,t.arguments,r.catalog_sha256,r.commit_sha,
	r.cancel_requested_at IS NOT NULL,a.state,a.board_lease_id::text,
	a.started_at,a.deadline_at,a.child_exit_code,a.hit_deadline,a.evidence_complete,a.result_reason,
	l.generation,l.holder_id,l.state
	FROM task_attempts a JOIN tasks t ON t.id=a.task_id JOIN runs r ON r.id=t.run_id
	JOIN board_leases l ON l.id=a.board_lease_id
	WHERE a.id=$1 FOR UPDATE OF a,t,r,l`, in.AttemptID).Scan(
		&taskID, &selectedRunID, &runActor, &taskName, &scope, &taskDeadline,
		&rawArguments, &runCatalog, &runCommit, &runCancelled, &attemptState, &leaseID, &attemptStarted,
		&attemptDeadline, &storedExit, &storedTimedOut, &storedEvidence, &storedReason,
		&generation, &holderID, &leaseState)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: read board HIL completion target: %v", ErrUnavailable, err)
	}
	if selectedRunID != runID {
		return fmt.Errorf("%w: HIL task run changed during completion", ErrConflict)
	}
	if runCancelled && in.Result == "preempted" {
		in.Result = "cancelled"
	}
	definition, found := definitions.Task(taskName)
	if !found || definition.Scope != "hil" || definition.BoardPolicy != "exclusive" ||
		definition.HIL == nil || definition.HIL.BoardID != actor.boardID ||
		definition.DeadlineSeconds != taskDeadline || runCatalog != definitions.Digest() || runCommit != trustedCommit ||
		scope != "hil" || !validHILTerminalState(attemptState) || leaseID != in.LeaseID ||
		generation <= 0 || uint64(generation) != in.Generation || holderID != runActor ||
		(leaseState != "active" && leaseState != "ended") {
		return fmt.Errorf("%w: HIL attempt, lease, or catalog identity mismatch", ErrConflict)
	}
	var stored struct {
		Arguments []string         `json:"argv"`
		HIL       *catalog.HILTask `json:"hil"`
	}
	decoder := json.NewDecoder(bytes.NewReader(rawArguments))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&stored) != nil || len(stored.Arguments) != 0 || stored.HIL == nil || *stored.HIL != *definition.HIL {
		return fmt.Errorf("%w: persisted HIL definition changed", ErrConflict)
	}
	var wasClaimed bool
	if err := tx.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM audit
		WHERE actor_id=$1 AND action='board.hil.attempt_claimed' AND target_type='attempt'
		AND target_id=$2 AND reason->>'lease_id'=$3)`, actor.id, in.AttemptID, in.LeaseID).Scan(&wasClaimed); err != nil {
		return fmt.Errorf("%w: verify HIL claim audit: %v", ErrUnavailable, err)
	}
	if !wasClaimed {
		return fmt.Errorf("%w: board agent did not claim this HIL attempt", ErrDenied)
	}
	if in.Result == "succeeded" && len(in.Steps) != len(definition.Steps) {
		return fmt.Errorf("%w: successful HIL completion lacks step evidence", ErrInvalid)
	}
	if len(in.Steps) > len(definition.Steps) {
		return fmt.Errorf("%w: HIL completion has unexpected steps", ErrInvalid)
	}
	var previousEnd time.Time
	for ordinal, step := range in.Steps {
		if step.Key != definition.Steps[ordinal].Name || step.StartedAt.IsZero() ||
			step.EndedAt.Before(step.StartedAt) || step.StartedAt.Before(attemptStarted.Add(-5*time.Second)) ||
			step.EndedAt.After(attemptDeadline.Add(5*time.Second)) || step.DurationNS <= 0 ||
			step.DurationNS > (step.EndedAt.Sub(step.StartedAt)+5*time.Second).Nanoseconds() ||
			(!previousEnd.IsZero() && step.StartedAt.Before(previousEnd)) ||
			(step.State != "succeeded" && step.State != "failed" && step.State != "timed_out" && step.State != "cancelled") {
			return fmt.Errorf("%w: invalid HIL step evidence at ordinal %d", ErrInvalid, ordinal)
		}
		if in.Result == "succeeded" && (step.State != "succeeded" || step.ChildExitCode == nil || *step.ChildExitCode != 0) {
			return fmt.Errorf("%w: successful HIL result has unsuccessful step", ErrConflict)
		}
		var segmentOutcome string
		var segmentStarted, segmentEnded time.Time
		err := tx.QueryRow(ctx, `SELECT outcome,started_at,ended_at FROM board_segments
			WHERE board_id=$1 AND lease_id=$2 AND generation=$3 AND attempt_id=$4
			AND actor_id=$5 AND segment_key=$6 AND ended_at IS NOT NULL`,
			actor.boardID, in.LeaseID, int64(in.Generation), in.AttemptID, actor.id, step.Key).
			Scan(&segmentOutcome, &segmentStarted, &segmentEnded)
		if errors.Is(err, pgx.ErrNoRows) {
			return fmt.Errorf("%w: HIL step %s lacks its matching closed durable board segment", ErrConflict, step.Key)
		}
		if err != nil {
			return fmt.Errorf("%w: query durable board segment: %v", ErrUnavailable, err)
		}
		if step.StartedAt.Before(segmentStarted.Add(-5*time.Second)) ||
			step.EndedAt.After(segmentEnded.Add(5*time.Second)) ||
			(step.State == "succeeded" && segmentOutcome != "completed") ||
			(step.State != "succeeded" && segmentOutcome == "completed") {
			return fmt.Errorf("%w: HIL step %s contradicts its durable board segment", ErrConflict, step.Key)
		}
		if step.Key == definition.HIL.ObservationStep {
			if _, err := hilWorkloadForSession(ctx, tx, in.LeaseID, *definition.HIL, segmentStarted, segmentEnded); err != nil {
				return fmt.Errorf("%w: HIL observation segment is outside its recorded board session: %v", ErrConflict, err)
			}
		}
		previousEnd = step.EndedAt
	}
	var openSegment bool
	if err := tx.QueryRow(ctx, "SELECT EXISTS (SELECT 1 FROM board_segments WHERE board_id=$1 AND ended_at IS NULL)", actor.boardID).Scan(&openSegment); err != nil {
		return fmt.Errorf("%w: check for open board segment: %v", ErrUnavailable, err)
	}
	if openSegment {
		return fmt.Errorf("%w: HIL attempt still has an open board segment", ErrConflict)
	}
	if attemptState != "running" {
		return completeHILReplay(ctx, tx, in, attemptState, taskID, runID, taskResultForHIL(in, runCancelled),
			storedExit, storedTimedOut, storedEvidence, storedReason)
	}
	var currentState string
	var version int64
	if err := tx.QueryRow(ctx, "SELECT state,version FROM task_attempts WHERE id=$1 FOR UPDATE", in.AttemptID).Scan(&currentState, &version); err != nil || currentState != "running" {
		return fmt.Errorf("%w: HIL attempt is no longer running", ErrConflict)
	}
	for ordinal, step := range in.Steps {
		phase := "execute"
		if step.Key == definition.HIL.ObservationStep {
			phase = "hil_observe"
		}
		_, err := tx.Exec(ctx, `INSERT INTO task_steps
		(attempt_id,step_key,ordinal,phase,started_at,ended_at,duration_ns,state,child_exit_code)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9)`, in.AttemptID, step.Key, ordinal,
			phase, step.StartedAt, step.EndedAt, step.DurationNS, step.State, step.ChildExitCode)
		if err != nil {
			return fmt.Errorf("%w: persist HIL step %s: %v", ErrConflict, step.Key, err)
		}
	}
	tag, err := tx.Exec(ctx, `UPDATE task_attempts SET state=$2,ended_at=clock_timestamp(),
	child_exit_code=$3,hit_deadline=$4,evidence_complete=$5,result_reason=$6,version=version+1
	WHERE id=$1 AND version=$7 AND state='running'`, in.AttemptID, in.Result,
		in.ChildExitCode, in.HitDeadline, in.EvidenceComplete, nullable(in.Reason), version)
	if err != nil || tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: finish HIL attempt: %v", ErrConflict, err)
	}
	var taskState string
	if err := tx.QueryRow(ctx, "SELECT state FROM tasks WHERE id=$1 FOR UPDATE", taskID).Scan(&taskState); err != nil || taskState != "running" {
		return fmt.Errorf("%w: HIL task is no longer running", ErrConflict)
	}
	taskResult := taskResultForHIL(in, runCancelled)
	requeue := taskResult == "scheduled"
	if requeue {
		if _, err := tx.Exec(ctx, `UPDATE tasks SET state='scheduled',started_at=NULL,ended_at=NULL,
			enqueued_at=clock_timestamp(),version=version+1 WHERE id=$1 AND state='running'`, taskID); err != nil {
			return fmt.Errorf("%w: requeue preempted HIL task: %v", ErrUnavailable, err)
		}
	} else {
		if _, err := tx.Exec(ctx, "UPDATE tasks SET state=$2,ended_at=clock_timestamp(),version=version+1 WHERE id=$1 AND state='running'", taskID, taskResult); err != nil {
			return fmt.Errorf("%w: finish HIL task: %v", ErrUnavailable, err)
		}
	}
	if requeue {
		if err := appendAudit(ctx, tx, actor.id, "task.requeued", "task", taskID, "ok", "running", "scheduled", runID,
			map[string]any{"attempt_id": in.AttemptID, "reason": "cooperative_board_yield", "lease_id": in.LeaseID}); err != nil {
			return fmt.Errorf("%w: audit HIL requeue: %v", ErrUnavailable, err)
		}
		if err := appendEvent(ctx, tx, runID, "task.requeued", map[string]any{
			"task_id": taskID, "attempt_id": in.AttemptID, "reason": "cooperative_board_yield"}); err != nil {
			return fmt.Errorf("%w: record HIL requeue event: %v", ErrUnavailable, err)
		}
	} else {
		if err := appendAudit(ctx, tx, actor.id, "task.attempt.finished", "task", taskID, "ok", "running", taskResult, runID,
			map[string]any{"attempt_id": in.AttemptID, "lease_id": in.LeaseID, "generation": in.Generation,
				"result": in.Result, "evidence_complete": in.EvidenceComplete}); err != nil {
			return fmt.Errorf("%w: audit HIL completion: %v", ErrUnavailable, err)
		}
		if err := appendEvent(ctx, tx, runID, "attempt.finished", map[string]any{
			"task_id": taskID, "attempt_id": in.AttemptID, "result": taskResult}); err != nil {
			return fmt.Errorf("%w: record HIL completion event: %v", ErrUnavailable, err)
		}
		if taskResult != "succeeded" {
			if err := skipDescendants(ctx, tx, runID, taskID, actor.id); err != nil {
				return err
			}
		}
		if err := closeRunIfTerminal(ctx, tx, runID, actor.id); err != nil {
			return err
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit HIL completion: %v", ErrUnavailable, err)
	}
	return nil
}

func validHILTerminalState(state string) bool {
	switch state {
	case "running", "succeeded", "failed", "timed_out", "cancelled", "preempted":
		return true
	default:
		return false
	}
}

func taskResultForHIL(in BoardHILCompletion, runCancelled bool) string {
	if in.Result == "preempted" {
		if runCancelled {
			return "cancelled"
		}
		return "scheduled"
	}
	if in.Result == "succeeded" && !in.EvidenceComplete {
		return "failed"
	}
	return in.Result
}

func completeHILReplay(ctx context.Context, tx pgx.Tx, in BoardHILCompletion, attemptState,
	taskID, runID, taskResult string, exit sql.NullInt32, hitDeadline, evidence bool,
	reason sql.NullString) error {
	expectedState := in.Result
	if expectedState == "preempted" {
		expectedState = "preempted"
	}
	if attemptState != expectedState || exit.Valid != (in.ChildExitCode != nil) ||
		(exit.Valid && int(exit.Int32) != *in.ChildExitCode) || hitDeadline != in.HitDeadline ||
		evidence != in.EvidenceComplete || reason.Valid != (in.Reason != "") ||
		(reason.Valid && reason.String != in.Reason) {
		return fmt.Errorf("%w: HIL completion retry changed terminal result", ErrConflict)
	}
	var actualTaskState string
	if err := tx.QueryRow(ctx, "SELECT state FROM tasks WHERE id=$1", taskID).Scan(&actualTaskState); err != nil {
		return fmt.Errorf("%w: read HIL completion retry state: %v", ErrUnavailable, err)
	}
	if actualTaskState != taskResult {
		return fmt.Errorf("%w: HIL completion retry changed task state", ErrConflict)
	}
	rows, err := tx.Query(ctx, `SELECT step_key,started_at,ended_at,duration_ns,state,child_exit_code
		FROM task_steps WHERE attempt_id=$1 ORDER BY ordinal`, in.AttemptID)
	if err != nil {
		return fmt.Errorf("%w: read HIL completion retry steps: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	index := 0
	for rows.Next() {
		if index >= len(in.Steps) {
			return fmt.Errorf("%w: HIL completion retry changed step count", ErrConflict)
		}
		var key, state string
		var startedAt, endedAt time.Time
		var duration int64
		var childExit sql.NullInt32
		if err := rows.Scan(&key, &startedAt, &endedAt, &duration, &state, &childExit); err != nil {
			return fmt.Errorf("%w: scan HIL completion retry step: %v", ErrUnavailable, err)
		}
		expected := in.Steps[index]
		if key != expected.Key || !startedAt.Equal(expected.StartedAt) || !endedAt.Equal(expected.EndedAt) ||
			duration != expected.DurationNS || state != expected.State ||
			childExit.Valid != (expected.ChildExitCode != nil) ||
			(childExit.Valid && int(childExit.Int32) != *expected.ChildExitCode) {
			return fmt.Errorf("%w: HIL completion retry changed step evidence", ErrConflict)
		}
		index++
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("%w: iterate HIL completion retry steps: %v", ErrUnavailable, err)
	}
	rows.Close()
	if index != len(in.Steps) {
		return fmt.Errorf("%w: HIL completion retry omitted step evidence", ErrConflict)
	}
	return tx.Commit(ctx)
}
