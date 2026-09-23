package store

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
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

// BoardHILAssignment is a server-selected, lease-bound HIL task and its pinned
// source identity. It contains no caller-supplied command text.
type BoardHILAssignment struct {
	Attempt         Attempt      `json:"attempt"`
	Task            catalog.Task `json:"task"`
	Args            []string     `json:"args"`
	RunID           string       `json:"run_id"`
	Repository      string       `json:"repository"`
	Branch          string       `json:"branch"`
	CommitSHA       string       `json:"commit_sha"`
	SnapshotSHA256  string       `json:"snapshot_sha256"`
	SourceAlgorithm string       `json:"source_algorithm"`
	CatalogSHA256   string       `json:"catalog_sha256"`
}

// ClaimNextBoardHILAttempt selects the oldest eligible HIL task for the
// current lease holder, then binds its attempt to this board-agent identity.
func (s *Store) ClaimNextBoardHILAttempt(ctx context.Context, actor BoardActor, leaseID string,
	facts StartAttemptInput, definitions *catalog.Catalog, trustedCommit string) (*BoardHILAssignment, error) {
	if s == nil || s.pool == nil || actor.kind != "board_agent" || actor.role != "board_agent" ||
		!validBoardID(actor.boardID) || !ValidID(leaseID) || definitions == nil ||
		definitions.Digest() == "" || !commitSHA.MatchString(trustedCommit) {
		return nil, fmt.Errorf("%w: board HIL queue claim arguments", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return nil, fmt.Errorf("%w: begin board HIL queue claim: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return nil, err
	}
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 1))", actor.boardID); err != nil {
		return nil, fmt.Errorf("%w: board HIL queue lock: %v", ErrUnavailable, err)
	}
	var holderID string
	err = tx.QueryRow(ctx, `SELECT holder_id FROM board_leases
        WHERE id=$1 AND board_id=$2 AND state='active' AND yield_requested_at IS NULL
          AND expires_at>clock_timestamp()`, leaseID, actor.boardID).Scan(&holderID)
	if err != nil {
		if err == pgx.ErrNoRows {
			return nil, fmt.Errorf("%w: board lease is not claimable", ErrConflict)
		}
		return nil, fmt.Errorf("%w: read HIL lease holder: %v", ErrUnavailable, err)
	}
	// Claims are idempotent across a lost HTTP response. A retry under the
	// same live lease returns its already-running attempt instead of observing
	// an empty scheduled queue and leaving the agent unable to continue.
	var existingRaw []byte
	var existing TaskHILAttempt
	var existingTaskName string
	var existingTaskVersion, existingDeadline int
	var existingCommit, existingSnapshot, existingCatalog string
	err = tx.QueryRow(ctx, `SELECT a.id::text,a.task_id::text,a.attempt_no,a.state,a.started_at,a.deadline_at,
		t.name,t.version,t.deadline_seconds,t.arguments,r.id::text,r.repository,r.branch,r.commit_sha,
		r.snapshot_sha256,r.catalog_sha256
		FROM task_attempts a JOIN tasks t ON t.id=a.task_id JOIN runs r ON r.id=t.run_id
		WHERE a.board_lease_id=$1 AND a.state='running' AND t.scope='hil' AND r.actor_id=$2
		  AND EXISTS (SELECT 1 FROM audit u WHERE u.actor_id=$3
		    AND u.action='board.hil.attempt_claimed' AND u.target_type='attempt'
		    AND u.target_id=a.id::text AND u.reason->>'lease_id'=$1)
		ORDER BY a.started_at DESC LIMIT 1`, leaseID, holderID, actor.id).Scan(
		&existing.ID, &existing.TaskID, &existing.AttemptNo, &existing.State, &existing.StartedAt,
		&existing.DeadlineAt, &existingTaskName, &existingTaskVersion, &existingDeadline,
		&existingRaw, &existing.RunID, &existing.Repository, &existing.Branch, &existingCommit,
		&existingSnapshot, &existingCatalog)
	if err == nil {
		definition, found := definitions.Task(existingTaskName)
		if !found || definition.Scope != "hil" || definition.BoardPolicy != "exclusive" ||
			definition.HIL == nil || definition.HIL.BoardID != actor.boardID ||
			!definition.SupportsOS("linux") || definition.Version != existingTaskVersion ||
			definition.DeadlineSeconds != existingDeadline || existingCatalog != definitions.Digest() ||
			existingCommit != trustedCommit {
			return nil, fmt.Errorf("%w: active HIL attempt differs from the current reviewed catalog or trusted commit", ErrConflict)
		}
		var persisted struct {
			Args []string         `json:"argv"`
			HIL  *catalog.HILTask `json:"hil"`
		}
		decoder := json.NewDecoder(bytes.NewReader(existingRaw))
		decoder.DisallowUnknownFields()
		if decoder.Decode(&persisted) != nil || persisted.HIL == nil ||
			*persisted.HIL != *definition.HIL || definition.ValidateArguments(persisted.Args) != nil {
			return nil, fmt.Errorf("%w: running HIL assignment no longer matches its catalog contract", ErrConflict)
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, fmt.Errorf("%w: close idempotent HIL claim: %v", ErrUnavailable, err)
		}
		return &BoardHILAssignment{Attempt: existing.Attempt, Task: definition,
			Args: append([]string(nil), persisted.Args...), RunID: existing.RunID,
			Repository: existing.Repository, Branch: existing.Branch, CommitSHA: existingCommit,
			SnapshotSHA256: existingSnapshot, SourceAlgorithm: source.Algorithm,
			CatalogSHA256: existingCatalog}, nil
	} else if err != pgx.ErrNoRows {
		return nil, fmt.Errorf("%w: read existing HIL assignment: %v", ErrUnavailable, err)
	}
	var selected BoardHILAssignment
	var taskID, taskName string
	var taskVersion, deadlineSeconds int
	var rawArguments []byte
	err = tx.QueryRow(ctx, `SELECT t.id::text,t.name,t.version,t.deadline_seconds,t.arguments,
          r.id::text,r.repository,r.branch,r.commit_sha,r.snapshot_sha256,r.catalog_sha256
        FROM tasks t JOIN runs r ON r.id=t.run_id
        WHERE t.scope='hil' AND t.state='scheduled' AND r.actor_id=$1
          AND r.state IN ('queued','running') AND r.cancel_requested_at IS NULL
          AND r.catalog_sha256=$2 AND r.commit_sha=$3
          AND t.arguments->'hil'->>'board_id'=$4
          AND NOT EXISTS (SELECT 1 FROM task_edges e JOIN tasks d
            ON d.id=e.depends_on_task_id WHERE e.task_id=t.id AND d.state<>'succeeded')
        ORDER BY t.enqueued_at,t.id
        LIMIT 1 FOR UPDATE OF t,r SKIP LOCKED`,
		holderID, definitions.Digest(), trustedCommit, actor.boardID).
		Scan(&taskID, &taskName, &taskVersion, &deadlineSeconds, &rawArguments,
			&selected.RunID, &selected.Repository, &selected.Branch, &selected.CommitSHA,
			&selected.SnapshotSHA256, &selected.CatalogSHA256)
	if err != nil {
		if err == pgx.ErrNoRows {
			if err := tx.Commit(ctx); err != nil {
				return nil, fmt.Errorf("%w: close empty HIL claim: %v", ErrUnavailable, err)
			}
			return nil, nil
		}
		return nil, fmt.Errorf("%w: select HIL task: %v", ErrUnavailable, err)
	}
	definition, found := definitions.Task(taskName)
	if !found || definition.Scope != "hil" || definition.BoardPolicy != "exclusive" ||
		definition.HIL == nil || definition.HIL.BoardID != actor.boardID ||
		!definition.SupportsOS("linux") || definition.Version != taskVersion ||
		definition.DeadlineSeconds != deadlineSeconds {
		return nil, fmt.Errorf("%w: HIL task differs from the current reviewed catalog", ErrConflict)
	}
	var persisted struct {
		Args []string         `json:"argv"`
		HIL  *catalog.HILTask `json:"hil"`
	}
	decoder := json.NewDecoder(bytes.NewReader(rawArguments))
	decoder.DisallowUnknownFields()
	if decoder.Decode(&persisted) != nil || persisted.HIL == nil ||
		*persisted.HIL != *definition.HIL || definition.ValidateArguments(persisted.Args) != nil {
		return nil, fmt.Errorf("%w: persisted HIL contract differs from catalog", ErrConflict)
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, fmt.Errorf("%w: close HIL selection: %v", ErrUnavailable, err)
	}

	facts.TaskID, facts.BoardLeaseID = taskID, leaseID
	attempt, err := s.StartBoardHILAttempt(ctx, actor, taskID, leaseID, facts)
	if err != nil {
		return nil, err
	}
	selected.Attempt = attempt
	selected.SourceAlgorithm = source.Algorithm
	selected.Task = definition
	selected.Args = append([]string(nil), persisted.Args...)
	selected.CatalogSHA256 = definitions.Digest()
	return &selected, nil
}

type TaskHILAttempt struct {
	Attempt
	RunID      string
	Repository string
	Branch     string
}
