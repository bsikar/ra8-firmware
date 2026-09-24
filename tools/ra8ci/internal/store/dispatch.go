package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"hash"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
	"github.com/jackc/pgx/v5"
)

type agentIdentity struct {
	ID        string
	Principal string
	HostClass string
	OS        string
	State     string
	Capacity  int
}

// agentForCertificate authenticates a live, enrolled mTLS identity inside the
// same transaction that changes task state. A revoked principal cannot submit
// fresh evidence, even when it knows a previously issued assignment token.
func agentForCertificate(ctx context.Context, tx pgx.Tx, certDER []byte) (agentIdentity, error) {
	if len(certDER) == 0 {
		return agentIdentity{}, ErrDenied
	}
	sum := sha256.Sum256(certDER)
	var agent agentIdentity
	err := tx.QueryRow(ctx, `SELECT a.id::text, a.principal_id, a.host_class,
		COALESCE(a.capabilities->>'os',''), a.state, a.capacity FROM api_principals p
		JOIN agents a ON a.principal_id=p.principal_id
		WHERE p.cert_sha256=$1 AND p.kind='agent' AND p.revoked_at IS NULL
		AND p.expires_at>clock_timestamp() AND a.revoked_at IS NULL
		AND a.state IN ('enrolled','healthy','draining')`, hex.EncodeToString(sum[:])).Scan(
		&agent.ID, &agent.Principal, &agent.HostClass, &agent.OS, &agent.State, &agent.Capacity)
	if errors.Is(err, pgx.ErrNoRows) {
		return agentIdentity{}, ErrDenied
	}
	if err != nil {
		return agentIdentity{}, fmt.Errorf("%w: agent authentication: %v", ErrUnavailable, err)
	}
	if agent.OS != "linux" && agent.OS != "windows" {
		return agentIdentity{}, ErrDenied
	}
	if (agent.HostClass == "linux-vm" || agent.HostClass == "runner") && agent.OS != "linux" {
		return agentIdentity{}, ErrDenied
	}
	if agent.HostClass == "windows-vm" && agent.OS != "windows" {
		return agentIdentity{}, ErrDenied
	}
	return agent, nil
}

func agentMayExecute(ctx context.Context, tx pgx.Tx, agent agentIdentity, repository string) error {
	var allowed bool
	err := tx.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM api_grants
		WHERE principal_id=$1 AND repository=$2 AND role='agent_executor')`, agent.Principal, repository).Scan(&allowed)
	if err != nil {
		return fmt.Errorf("%w: agent grant: %v", ErrUnavailable, err)
	}
	if !allowed {
		return ErrDenied
	}
	return nil
}

// ClaimAgentTask issues at most one fenced assignment to an idle agent. The
// checkout, task definition and OS are rechecked by the agent before ACK.
func (s *Store) ClaimAgentTask(ctx context.Context, certDER []byte, facts protocol.HostFacts, tasks *catalog.Catalog, trustedCommit string) (*protocol.Assignment, error) {
	if err := facts.Validate(); err != nil || tasks == nil || tasks.Digest() == "" || !protocol.ValidCommit(trustedCommit) {
		return nil, fmt.Errorf("%w: claim facts or catalog", ErrInvalid)
	}
	var names []string
	for _, name := range tasks.Names() {
		definition, _ := tasks.Task(name)
		if definition.Scope == "safe-local-read-only" && definition.BoardPolicy == "none" &&
			len(definition.Steps) > 0 && definition.SupportsOS(facts.OS) {
			names = append(names, name)
		}
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return nil, fmt.Errorf("%w: begin claim: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return nil, err
	}
	if _, err := tx.Exec(ctx, `SELECT pg_advisory_xact_lock(hashtextextended($1,2))`, agent.ID); err != nil {
		return nil, fmt.Errorf("%w: lock agent claim: %v", ErrUnavailable, err)
	}
	if agent.OS != facts.OS {
		return nil, ErrDenied
	}
	if agent.State != "healthy" || agent.Capacity < 1 {
		return nil, ErrDenied
	}
	if len(names) == 0 {
		return nil, nil
	}
	var prior protocol.Assignment
	var priorState string
	err = tx.QueryRow(ctx, `SELECT a.id::text, a.assignment_id::text,
		a.assignment_version, a.fencing_token, a.state, t.name,
		r.catalog_sha256, r.commit_sha, r.snapshot_sha256, a.deadline_at
		FROM task_attempts a JOIN tasks t ON t.id=a.task_id
		JOIN runs r ON r.id=t.run_id
		WHERE a.agent_id=$1 AND a.state IN ('issued','acknowledged','running')
		ORDER BY a.issued_at LIMIT 1 FOR UPDATE OF a`, agent.ID).Scan(
		&prior.AttemptID, &prior.AssignmentID, &prior.AssignmentVersion,
		&prior.FencingToken, &priorState, &prior.Task.Name,
		&prior.CatalogSHA256, &prior.Source.Commit,
		&prior.Source.SnapshotSHA256, &prior.DeadlineAt)
	if err == nil {
		var priorRepository string
		lookupErr := tx.QueryRow(ctx, `SELECT r.repository FROM task_attempts a
			JOIN tasks t ON t.id=a.task_id JOIN runs r ON r.id=t.run_id
			WHERE a.id=$1`, prior.AttemptID).Scan(&priorRepository)
		if lookupErr != nil {
			return nil, fmt.Errorf("%w: replay repository: %v", ErrUnavailable, lookupErr)
		}
		if err := agentMayExecute(ctx, tx, agent, priorRepository); err != nil {
			return nil, err
		}
		if priorState != "issued" || prior.CatalogSHA256 != tasks.Digest() ||
			prior.Source.Commit != trustedCommit {
			return nil, nil
		}
		definition, found := tasks.Task(prior.Task.Name)
		if !found || definition.Scope != "safe-local-read-only" || len(definition.Steps) == 0 ||
			definition.BoardPolicy != "none" || !definition.SupportsOS(agent.OS) {
			return nil, ErrConflict
		}
		prior.Task.Version = definition.Version
		prior.SchemaVersion = protocol.Version
		prior.Source.Algorithm = source.Algorithm
		var databaseNow time.Time
		if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
			return nil, fmt.Errorf("%w: replay clock: %v", ErrUnavailable, err)
		}
		prior.RemainingMS = prior.DeadlineAt.Sub(databaseNow).Milliseconds()
		if prior.RemainingMS < 1 {
			return nil, nil
		}
		if err := tx.Commit(ctx); err != nil {
			return nil, fmt.Errorf("%w: replay assignment: %v", ErrUnavailable, err)
		}
		return &prior, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return nil, fmt.Errorf("%w: check agent capacity: %v", ErrUnavailable, err)
	}
	var runID, runState, repository, commitSHA, snapshotSHA, catalogSHA string
	err = tx.QueryRow(ctx, `SELECT r.id::text, r.state, r.repository, r.commit_sha,
		r.snapshot_sha256, r.catalog_sha256 FROM runs r
		WHERE r.state IN ('queued','running') AND r.cancel_requested_at IS NULL
		AND r.catalog_sha256=$1 AND r.commit_sha=$4
		AND EXISTS (SELECT 1 FROM api_grants g WHERE g.principal_id=$2
			AND g.repository=r.repository AND g.role='agent_executor')
		AND EXISTS (SELECT 1 FROM tasks t WHERE t.run_id=r.id AND t.state='scheduled'
			AND t.scope='safe-local-read-only' AND t.name=ANY($3)
			AND NOT EXISTS (SELECT 1 FROM task_edges e JOIN tasks d
				ON d.id=e.depends_on_task_id WHERE e.task_id=t.id AND d.state<>'succeeded'))
		ORDER BY r.created_at, r.id LIMIT 1 FOR UPDATE OF r SKIP LOCKED`,
		tasks.Digest(), agent.Principal, names, trustedCommit).Scan(&runID, &runState,
		&repository, &commitSHA, &snapshotSHA, &catalogSHA)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("%w: select run: %v", ErrUnavailable, err)
	}
	if err := agentMayExecute(ctx, tx, agent, repository); err != nil {
		return nil, err
	}
	var taskID, taskName, taskState string
	var taskVersion int64
	var deadlineSeconds int
	err = tx.QueryRow(ctx, `SELECT t.id::text, t.name, t.state, t.version, t.deadline_seconds
		FROM tasks t WHERE t.run_id=$1 AND t.state='scheduled'
		AND t.scope='safe-local-read-only' AND t.name=ANY($2)
		AND NOT EXISTS (SELECT 1 FROM task_edges e JOIN tasks d
			ON d.id=e.depends_on_task_id WHERE e.task_id=t.id AND d.state<>'succeeded')
		ORDER BY t.enqueued_at, t.id LIMIT 1 FOR UPDATE OF t SKIP LOCKED`, runID, names).Scan(
		&taskID, &taskName, &taskState, &taskVersion, &deadlineSeconds)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("%w: select task: %v", ErrUnavailable, err)
	}
	definition, found := tasks.Task(taskName)
	if !found || definition.Scope != "safe-local-read-only" || definition.BoardPolicy != "none" ||
		len(definition.Steps) == 0 ||
		!definition.SupportsOS(facts.OS) || deadlineSeconds != definition.DeadlineSeconds {
		return nil, fmt.Errorf("%w: unreviewed task definition", ErrConflict)
	}
	attemptID, err := NewID()
	if err != nil {
		return nil, fmt.Errorf("%w: attempt ID: %v", ErrUnavailable, err)
	}
	assignmentID, err := NewID()
	if err != nil {
		return nil, fmt.Errorf("%w: assignment ID: %v", ErrUnavailable, err)
	}
	var attemptNo int
	err = tx.QueryRow(ctx, `SELECT COALESCE(MAX(attempt_no),0)+1
		FROM task_attempts WHERE task_id=$1`, taskID).Scan(&attemptNo)
	if err != nil {
		return nil, fmt.Errorf("%w: attempt number: %v", ErrUnavailable, err)
	}
	factsJSON, _ := json.Marshal(facts)
	var deadline time.Time
	err = tx.QueryRow(ctx, `INSERT INTO task_attempts
		(id, task_id, attempt_no, agent_id, assignment_id, assignment_version,
		 fencing_token, state, engine, host, host_cores, host_ram_bytes, host_load,
		 host_facts, started_at, deadline_at)
		VALUES ($1,$2,$3,$4,$5,1,$6,'issued','ra8ci-agent',$7,$8,$9,$10,$11,
		 clock_timestamp(),clock_timestamp()+($12 * interval '1 second'))
		RETURNING deadline_at`, attemptID, taskID, attemptNo, agent.ID, assignmentID,
		taskVersion+1, agent.HostClass, facts.Cores, facts.RAMBytes, facts.Load1,
		factsJSON, deadlineSeconds).Scan(&deadline)
	if err != nil {
		return nil, fmt.Errorf("%w: insert issued attempt: %v", ErrUnavailable, err)
	}
	if err := CheckTaskTransition(taskState, "running"); err != nil {
		return nil, err
	}
	tag, err := tx.Exec(ctx, `UPDATE tasks SET state='running', started_at=clock_timestamp(),
		version=version+1 WHERE id=$1 AND version=$2 AND state=$3`, taskID, taskVersion, taskState)
	if err != nil || tag.RowsAffected() != 1 {
		return nil, fmt.Errorf("%w: claim task: %v", ErrConflict, err)
	}
	// The candidate query takes queued and running runs alike, so the run is
	// started only from the state the machine has that edge out of. This was
	// an UPDATE fenced on state='queued' whose tag was discarded: the same
	// rule stated a second time in SQL, and never checked.
	if RunStartable(runState) {
		_, err = tx.Exec(ctx, `UPDATE runs SET state='running',
			started_at=COALESCE(started_at,clock_timestamp()), version=version+1
			WHERE id=$1 AND state=$2`, runID, runState)
		if err != nil {
			return nil, fmt.Errorf("%w: start run: %v", ErrUnavailable, err)
		}
	}
	if err := appendAudit(ctx, tx, agent.Principal, "task.assigned", "task", taskID,
		"ok", "scheduled", "running", runID,
		map[string]any{"attempt_id": attemptID, "assignment_id": assignmentID, "fence": taskVersion + 1}); err != nil {
		return nil, fmt.Errorf("%w: audit assignment: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "task.assigned", map[string]any{
		"task_id": taskID, "attempt_id": attemptID, "assignment_id": assignmentID}); err != nil {
		return nil, fmt.Errorf("%w: event assignment: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return nil, fmt.Errorf("%w: commit assignment: %v", ErrUnavailable, err)
	}
	var databaseNow time.Time
	if err := s.pool.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
		return nil, fmt.Errorf("%w: assignment clock: %v", ErrUnavailable, err)
	}
	remaining := deadline.Sub(databaseNow).Milliseconds()
	if remaining < 1 {
		remaining = 1
	}
	assignment := &protocol.Assignment{SchemaVersion: protocol.Version,
		AssignmentID: assignmentID, AttemptID: attemptID, AssignmentVersion: 1,
		FencingToken: taskVersion + 1, Task: protocol.TaskRef{Name: taskName, Version: definition.Version},
		CatalogSHA256: catalogSHA, Source: protocol.SourceRef{Algorithm: source.Algorithm,
			Commit: commitSHA, SnapshotSHA256: snapshotSHA}, DeadlineAt: deadline,
		RemainingMS: remaining}
	return assignment, nil
}

type agentAttempt struct {
	AgentID         string
	TaskID          string
	RunID           string
	Repository      string
	State           string
	CatalogSHA      string
	SnapshotSHA     string
	DeadlineAt      time.Time
	IssuedAt        time.Time
	DeadlineSeconds int
	Version         int64
	Fence           int64
}

func lockAgentAttempt(ctx context.Context, tx pgx.Tx, agent agentIdentity, assignmentID, attemptID string, version, fence int64) (agentAttempt, error) {
	if !protocol.ValidID(assignmentID) || !protocol.ValidID(attemptID) || version < 1 || fence < 1 {
		return agentAttempt{}, fmt.Errorf("%w: grant identity", ErrInvalid)
	}
	var runID string
	err := tx.QueryRow(ctx, `SELECT t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id WHERE a.id=$1 AND a.assignment_id=$2`,
		attemptID, assignmentID).Scan(&runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return agentAttempt{}, ErrNotFound
	}
	if err != nil {
		return agentAttempt{}, fmt.Errorf("%w: locate agent attempt: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return agentAttempt{}, fmt.Errorf("%w: lock run: %v", ErrUnavailable, err)
	}
	var attempt agentAttempt
	err = tx.QueryRow(ctx, `SELECT a.agent_id::text, a.task_id::text, t.run_id::text,
		r.repository, a.state, r.catalog_sha256, r.snapshot_sha256,
		a.deadline_at, a.issued_at, t.deadline_seconds,
		a.assignment_version, a.fencing_token
		FROM task_attempts a JOIN tasks t ON t.id=a.task_id
		JOIN runs r ON r.id=t.run_id
		WHERE a.id=$1 AND a.assignment_id=$2 FOR UPDATE OF a`,
		attemptID, assignmentID).Scan(&attempt.AgentID, &attempt.TaskID, &attempt.RunID,
		&attempt.Repository, &attempt.State, &attempt.CatalogSHA, &attempt.SnapshotSHA,
		&attempt.DeadlineAt, &attempt.IssuedAt, &attempt.DeadlineSeconds,
		&attempt.Version, &attempt.Fence)
	if errors.Is(err, pgx.ErrNoRows) {
		return agentAttempt{}, ErrNotFound
	}
	if err != nil {
		return agentAttempt{}, fmt.Errorf("%w: find agent attempt: %v", ErrUnavailable, err)
	}
	if attempt.AgentID != agent.ID || attempt.Version != version || attempt.Fence != fence {
		return agentAttempt{}, ErrConflict
	}
	if err := agentMayExecute(ctx, tx, agent, attempt.Repository); err != nil {
		return agentAttempt{}, err
	}
	return attempt, nil
}

// AcknowledgeAgentAssignment transitions issued -> running only after the
// agent proves it verified the exact source and catalog named in the grant.
func (s *Store) AcknowledgeAgentAssignment(ctx context.Context, certDER []byte, ack protocol.Ack) error {
	if err := ack.Validate(); err != nil {
		return fmt.Errorf("%w: acknowledgment", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin acknowledgment: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return err
	}
	if ack.HostFacts.OS != agent.OS {
		return ErrDenied
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, ack.AssignmentID, ack.AttemptID,
		ack.AssignmentVersion, ack.FencingToken)
	if err != nil {
		return err
	}
	if ack.CatalogSHA256 != attempt.CatalogSHA || ack.SourceSnapshotSHA256 != attempt.SnapshotSHA {
		return ErrConflict
	}
	if attempt.State != "issued" && attempt.State != "running" {
		return ErrConflict
	}
	if attempt.State == "issued" {
		var cancellationRequested bool
		if err := tx.QueryRow(ctx, `SELECT cancel_requested_at IS NOT NULL FROM runs WHERE id=$1`, attempt.RunID).Scan(&cancellationRequested); err != nil {
			return fmt.Errorf("%w: read run cancellation: %v", ErrUnavailable, err)
		}
		if cancellationRequested {
			if err := CheckAttemptTransition(attempt.State, "cancelled"); err != nil {
				return err
			}
			if _, err := tx.Exec(ctx, `UPDATE task_attempts SET state='cancelled', ended_at=clock_timestamp(),
				evidence_complete=true, result_reason='run_cancelled_before_ack', version=version+1
				WHERE id=$1 AND state=$2`, ack.AttemptID, attempt.State); err != nil {
				return fmt.Errorf("%w: cancel unacknowledged attempt: %v", ErrUnavailable, err)
			}
			var taskState string
			if err := tx.QueryRow(ctx, `SELECT state FROM tasks WHERE id=$1 FOR UPDATE`,
				attempt.TaskID).Scan(&taskState); err != nil {
				return fmt.Errorf("%w: read task before cancelling: %v", ErrUnavailable, err)
			}
			// This attempt is cancelled either way, but the task belongs to
			// the run: a sibling attempt may already have ended it, so the
			// machine decides whether there is still an edge to take. The
			// UPDATE was fenced on state='running' with its tag discarded,
			// so a task that had moved was left alone while the audit below
			// still recorded a running -> cancelled move.
			if err := CheckTaskTransition(taskState, "cancelled"); err == nil {
				if _, err := tx.Exec(ctx, `UPDATE tasks SET state='cancelled', ended_at=clock_timestamp(),
					version=version+1 WHERE id=$1 AND state=$2`, attempt.TaskID, taskState); err != nil {
					return fmt.Errorf("%w: cancel unacknowledged task: %v", ErrUnavailable, err)
				}
				if err := appendAudit(ctx, tx, agent.Principal, "task.cancelled_before_ack", "task",
					attempt.TaskID, "ok", taskState, "cancelled", attempt.RunID, map[string]any{"attempt_id": ack.AttemptID}); err != nil {
					return fmt.Errorf("%w: audit cancelled assignment: %v", ErrUnavailable, err)
				}
				if err := appendEvent(ctx, tx, attempt.RunID, "task.cancelled_before_ack", map[string]any{"task_id": attempt.TaskID, "attempt_id": ack.AttemptID}); err != nil {
					return fmt.Errorf("%w: event cancelled assignment: %v", ErrUnavailable, err)
				}
			}
			if err := closeRunIfTerminal(ctx, tx, attempt.RunID, agent.Principal); err != nil {
				return err
			}
			if err := tx.Commit(ctx); err != nil {
				return fmt.Errorf("%w: commit cancelled assignment: %v", ErrUnavailable, err)
			}
			return ErrConflict
		}
		var databaseNow time.Time
		if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
			return fmt.Errorf("%w: acknowledgment clock: %v", ErrUnavailable, err)
		}
		if !databaseNow.Before(attempt.DeadlineAt) {
			return ErrConflict
		}
		facts, _ := json.Marshal(ack.HostFacts)
		if err := CheckAttemptTransition(attempt.State, "running"); err != nil {
			return err
		}
		// lockAgentAttempt holds FOR UPDATE on this row, so the state cannot
		// move under us. The fence says which edge the write is taking rather
		// than setting running over whatever it finds.
		tag, err := tx.Exec(ctx, `UPDATE task_attempts SET state='running', host_cores=$2,
			host_ram_bytes=$3, host_load=$4, host_facts=$5,
			agent_last_heartbeat_at=clock_timestamp(), version=version+1
			WHERE id=$1 AND state=$6`,
			ack.AttemptID, ack.HostFacts.Cores, ack.HostFacts.RAMBytes,
			ack.HostFacts.Load1, facts, attempt.State)
		if err != nil {
			return fmt.Errorf("%w: acknowledge attempt: %v", ErrUnavailable, err)
		}
		if tag.RowsAffected() != 1 {
			return fmt.Errorf("%w: attempt moved under the acknowledgment", ErrConflict)
		}
		if err := recordResourceSample(ctx, tx, ack.AttemptID, ack.HostFacts); err != nil {
			return err
		}
		if err := appendAudit(ctx, tx, agent.Principal, "task.assignment.ack", "task",
			attempt.TaskID, "ok", attempt.State, "running", attempt.RunID,
			map[string]any{"attempt_id": ack.AttemptID}); err != nil {
			return fmt.Errorf("%w: audit acknowledgment: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit acknowledgment: %v", ErrUnavailable, err)
	}
	return nil
}

// SaveAgentLog enforces one global, gap-free sequence across both streams.
// Exact duplicate uploads are idempotent; altered replay is a conflict.
func (s *Store) SaveAgentLog(ctx context.Context, certDER []byte, chunk protocol.LogChunk) error {
	if err := chunk.Validate(); err != nil {
		return fmt.Errorf("%w: log chunk", ErrInvalid)
	}
	data, _ := base64.StdEncoding.DecodeString(chunk.DataBase64)
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin log: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return err
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, chunk.AssignmentID, chunk.AttemptID,
		chunk.AssignmentVersion, chunk.FencingToken)
	if err != nil {
		return err
	}
	if attempt.State != "running" {
		return ErrConflict
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
		return fmt.Errorf("%w: log clock: %v", ErrUnavailable, err)
	}
	if !databaseNow.Before(attempt.DeadlineAt.Add(agentEvidenceGrace)) {
		return ErrConflict
	}
	var maxSequence, totalBytes int64
	err = tx.QueryRow(ctx, `SELECT COALESCE(MAX(seq),0),
		COALESCE(SUM(octet_length(bytes)),0) FROM log_chunks
		WHERE attempt_id=$1`, chunk.AttemptID).Scan(&maxSequence, &totalBytes)
	if err != nil {
		return fmt.Errorf("%w: log sequence: %v", ErrUnavailable, err)
	}
	if chunk.Sequence <= maxSequence {
		var oldStream, oldSHA, oldStep string
		var oldData []byte
		err = tx.QueryRow(ctx, `SELECT stream, sha256, COALESCE(agent_step_key, ''), bytes FROM log_chunks
			WHERE attempt_id=$1 AND seq=$2`, chunk.AttemptID, chunk.Sequence).Scan(
			&oldStream, &oldSHA, &oldStep, &oldData)
		if err != nil || oldStream != chunk.Stream || oldSHA != chunk.SHA256 || oldStep != chunk.StepName || string(oldData) != string(data) {
			return ErrConflict
		}
	} else {
		if chunk.Sequence != maxSequence+1 || chunk.Sequence > maxAgentLogChunks ||
			totalBytes+int64(len(data)) > maxAgentLogBytes {
			return ErrConflict
		}
		_, err = tx.Exec(ctx, `INSERT INTO log_chunks
			(attempt_id, stream, seq, monotonic_offset_ns, sha256, bytes, agent_step_key)
			VALUES ($1,$2,$3,0,$4,$5,$6)`, chunk.AttemptID, chunk.Stream,
			chunk.Sequence, chunk.SHA256, data, chunk.StepName)
		if err != nil {
			return fmt.Errorf("%w: insert log: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit log: %v", ErrUnavailable, err)
	}
	return nil
}

// HeartbeatAgentAttempt returns only fenced, server-derived cancellation
// intent. A heartbeat never extends the task's immutable deadline.
func (s *Store) HeartbeatAgentAttempt(ctx context.Context, certDER []byte, heartbeat protocol.Heartbeat) (protocol.HeartbeatResponse, error) {
	if err := heartbeat.Validate(); err != nil {
		return protocol.HeartbeatResponse{}, fmt.Errorf("%w: heartbeat", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return protocol.HeartbeatResponse{}, fmt.Errorf("%w: begin heartbeat: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return protocol.HeartbeatResponse{}, err
	}
	if agent.OS != heartbeat.HostFacts.OS {
		return protocol.HeartbeatResponse{}, ErrDenied
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, heartbeat.AssignmentID,
		heartbeat.AttemptID, heartbeat.AssignmentVersion, heartbeat.FencingToken)
	if err != nil {
		return protocol.HeartbeatResponse{}, err
	}
	if attempt.State != "running" {
		return protocol.HeartbeatResponse{}, ErrConflict
	}
	facts, _ := json.Marshal(heartbeat.HostFacts)
	_, err = tx.Exec(ctx, `UPDATE task_attempts SET agent_last_heartbeat_at=clock_timestamp(),
		host_facts=$2 WHERE id=$1`, heartbeat.AttemptID, facts)
	if err != nil {
		return protocol.HeartbeatResponse{}, fmt.Errorf("%w: persist heartbeat: %v", ErrUnavailable, err)
	}
	if err := recordResourceSample(ctx, tx, heartbeat.AttemptID, heartbeat.HostFacts); err != nil {
		return protocol.HeartbeatResponse{}, err
	}
	var databaseNow time.Time
	var runCancelled bool
	if err := tx.QueryRow(ctx, `SELECT clock_timestamp(), cancel_requested_at IS NOT NULL
		FROM runs WHERE id=$1`, attempt.RunID).Scan(&databaseNow, &runCancelled); err != nil {
		return protocol.HeartbeatResponse{}, fmt.Errorf("%w: heartbeat clock or run cancellation: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return protocol.HeartbeatResponse{}, fmt.Errorf("%w: commit heartbeat: %v", ErrUnavailable, err)
	}
	return protocol.HeartbeatResponse{SchemaVersion: protocol.Version,
		AssignmentVersion: heartbeat.AssignmentVersion, FencingToken: heartbeat.FencingToken,
		Cancel: runCancelled || !databaseNow.Before(attempt.DeadlineAt)}, nil
}

// CompleteAgentAttempt persists the terminal result, all step timings, task
// state and run outcome in one transaction. It refuses missing log evidence.
func (s *Store) CompleteAgentAttempt(ctx context.Context, certDER []byte, receipt protocol.TerminalReceipt, tasks *catalog.Catalog) error {
	if err := receipt.Validate(); err != nil || tasks == nil {
		return fmt.Errorf("%w: terminal receipt", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin terminal receipt: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return err
	}
	if agent.OS != receipt.HostFactsAtStart.OS || agent.OS != receipt.HostFactsAtEnd.OS {
		return ErrDenied
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, receipt.AssignmentID,
		receipt.AttemptID, receipt.AssignmentVersion, receipt.FencingToken)
	if err != nil {
		return err
	}
	if attempt.State != "running" || receipt.CatalogSHA256 != attempt.CatalogSHA ||
		receipt.SourceSnapshotSHA256 != attempt.SnapshotSHA || attempt.CatalogSHA != tasks.Digest() {
		return ErrConflict
	}
	var taskName string
	err = tx.QueryRow(ctx, `SELECT name FROM tasks WHERE id=$1`, attempt.TaskID).Scan(&taskName)
	if err != nil {
		return fmt.Errorf("%w: terminal task: %v", ErrUnavailable, err)
	}
	definition, found := tasks.Task(taskName)
	if !found || len(receipt.Steps) > len(definition.Steps) || definition.Scope != "safe-local-read-only" {
		return ErrConflict
	}
	if receipt.Outcome == "succeeded" && len(receipt.Steps) != len(definition.Steps) {
		return ErrConflict
	}
	if len(receipt.Steps) < len(definition.Steps) && receipt.EvidenceComplete {
		return ErrConflict
	}
	for ordinal, step := range receipt.Steps {
		if step.Name != definition.Steps[ordinal].Name {
			return ErrConflict
		}
		if step.StartedAt.Before(receipt.StartedAt) || step.EndedAt.After(receipt.EndedAt) ||
			step.DurationNS > receipt.DurationNS ||
			(receipt.Outcome == "succeeded" && (step.ExitCode != 0 || step.TimedOut || step.Cancelled)) {
			return ErrConflict
		}
	}
	maxReportedDuration := time.Duration(attempt.DeadlineSeconds)*time.Second + agentEvidenceGrace
	if receipt.DurationNS > maxReportedDuration.Nanoseconds() ||
		receipt.EndedAt.Sub(receipt.StartedAt) > maxReportedDuration+5*time.Second {
		return ErrConflict
	}
	if err := verifyReceiptLogs(ctx, tx, receipt); err != nil {
		return err
	}
	if err := recordResourceSample(ctx, tx, receipt.AttemptID, receipt.HostFactsAtEnd); err != nil {
		return err
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
		return fmt.Errorf("%w: terminal clock: %v", ErrUnavailable, err)
	}
	if !databaseNow.Before(attempt.DeadlineAt.Add(agentEvidenceGrace)) {
		return ErrConflict
	}
	for ordinal, step := range receipt.Steps {
		stepState := "succeeded"
		if step.TimedOut {
			stepState = "timed_out"
		} else if step.Cancelled {
			stepState = "cancelled"
		} else if step.ExitCode != 0 {
			stepState = "failed"
		}
		_, err = tx.Exec(ctx, `INSERT INTO task_steps
			(attempt_id, step_key, ordinal, phase, started_at, ended_at,
			 duration_ns, state, child_exit_code)
			VALUES ($1,$2,$3,'execute',$4,$5,$6,$7,$8)`, receipt.AttemptID,
			step.Name, ordinal, step.StartedAt, step.EndedAt, step.DurationNS,
			stepState, step.ExitCode)
		if err != nil {
			return fmt.Errorf("%w: terminal step: %v", ErrConflict, err)
		}
	}
	// The downgrade of an unverifiable green is the store's rule, not this
	// call site's: taskResultFor states it once for both write sites. Unlike
	// FinishAttempt, where validAttemptResult makes the case unreachable, an
	// agent receipt can carry succeeded with incomplete evidence, and the
	// attempt is recorded with the same downgraded result as its task.
	result := taskResultFor(receipt.Outcome, receipt.EvidenceComplete)
	if err := CheckAttemptTransition(attempt.State, result); err != nil {
		return err
	}
	var taskState string
	if err := tx.QueryRow(ctx, `SELECT state FROM tasks WHERE id=$1 FOR UPDATE`,
		attempt.TaskID).Scan(&taskState); err != nil {
		return fmt.Errorf("%w: read task before finishing: %v", ErrUnavailable, err)
	}
	if err := CheckTaskTransition(taskState, result); err != nil {
		return err
	}
	_, err = tx.Exec(ctx, `UPDATE task_attempts SET state=$2,
		ended_at=clock_timestamp(), child_exit_code=$3, hit_deadline=$4,
		evidence_complete=$5, result_reason=$6, version=version+1
		WHERE id=$1 AND state=$7`, receipt.AttemptID, result,
		receipt.ChildExitCode, receipt.TimedOut, receipt.EvidenceComplete,
		nullable(receipt.ErrorCode), attempt.State)
	if err != nil {
		return fmt.Errorf("%w: terminal attempt: %v", ErrUnavailable, err)
	}
	// Fenced on the state the check was made against, and checked: a task
	// that moved under the receipt is a conflict, not a silent no-op with an
	// audit record claiming the move happened.
	tag, err := tx.Exec(ctx, `UPDATE tasks SET state=$2, ended_at=clock_timestamp(),
		version=version+1 WHERE id=$1 AND state=$3`, attempt.TaskID, result, taskState)
	if err != nil {
		return fmt.Errorf("%w: terminal task state: %v", ErrUnavailable, err)
	}
	if tag.RowsAffected() != 1 {
		return fmt.Errorf("%w: task moved under the terminal receipt", ErrConflict)
	}
	if err := appendAudit(ctx, tx, agent.Principal, "task.attempt.finished", "task",
		attempt.TaskID, "ok", taskState, result, attempt.RunID,
		map[string]any{"attempt_id": receipt.AttemptID, "evidence_complete": receipt.EvidenceComplete,
			"final_log_sequence": receipt.FinalLogSequence}); err != nil {
		return fmt.Errorf("%w: terminal audit: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, attempt.RunID, "attempt.finished", map[string]any{
		"task_id": attempt.TaskID, "attempt_id": receipt.AttemptID, "result": result}); err != nil {
		return fmt.Errorf("%w: terminal event: %v", ErrUnavailable, err)
	}
	if result != "succeeded" {
		if err := skipDescendants(ctx, tx, attempt.RunID, attempt.TaskID, agent.Principal); err != nil {
			return err
		}
	}
	if err := closeRunIfTerminal(ctx, tx, attempt.RunID, agent.Principal); err != nil {
		return err
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit terminal receipt: %v", ErrUnavailable, err)
	}
	return nil
}

const (
	agentEvidenceGrace = 60 * time.Second
	agentReaperGrace   = 65 * time.Second
	maxAgentLogBytes   = 64 << 20
	maxAgentLogChunks  = 4096
)

func validPartialStepEvidence(receipt protocol.TerminalReceipt, expectedSteps int) bool {
	if expectedSteps < 0 || len(receipt.Steps) > expectedSteps {
		return false
	}
	if len(receipt.Steps) == expectedSteps || !receipt.EvidenceComplete {
		return true
	}
	if len(receipt.Steps) == 0 {
		return false
	}
	last := receipt.Steps[len(receipt.Steps)-1]
	return receipt.TimedOut || receipt.Cancelled || last.TimedOut || last.Cancelled || last.ExitCode != 0
}

func verifyReceiptLogs(ctx context.Context, tx pgx.Tx, receipt protocol.TerminalReceipt) error {
	type stepLog struct {
		stdout, stderr           hash.Hash
		stdoutBytes, stderrBytes int64
	}
	logs := make(map[string]*stepLog, len(receipt.Steps))
	for _, step := range receipt.Steps {
		if step.Name == "" || logs[step.Name] != nil {
			return ErrConflict
		}
		logs[step.Name] = &stepLog{stdout: sha256.New(), stderr: sha256.New()}
	}
	rows, err := tx.Query(ctx, `SELECT seq, stream, agent_step_key, bytes FROM log_chunks WHERE attempt_id=$1 ORDER BY seq`, receipt.AttemptID)
	if err != nil {
		return fmt.Errorf("%w: read terminal logs: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	var last int64
	for rows.Next() {
		var seq int64
		var stream, stepName string
		var data []byte
		if err := rows.Scan(&seq, &stream, &stepName, &data); err != nil {
			return fmt.Errorf("%w: scan terminal log: %v", ErrUnavailable, err)
		}
		step := logs[stepName]
		if seq != last+1 || step == nil {
			return ErrConflict
		}
		switch stream {
		case "stdout":
			_, _ = step.stdout.Write(data)
			step.stdoutBytes += int64(len(data))
		case "stderr":
			_, _ = step.stderr.Write(data)
			step.stderrBytes += int64(len(data))
		default:
			return ErrConflict
		}
		last = seq
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("%w: terminal log rows: %v", ErrUnavailable, err)
	}
	if last != receipt.FinalLogSequence {
		return ErrConflict
	}
	if len(receipt.Steps) == 0 {
		if last != 0 {
			return ErrConflict
		}
		return nil
	}
	for _, summary := range receipt.Steps {
		step := logs[summary.Name]
		if step.stdoutBytes != summary.StdoutBytes || step.stderrBytes != summary.StderrBytes ||
			hex.EncodeToString(step.stdout.Sum(nil)) != summary.StdoutSHA256 ||
			hex.EncodeToString(step.stderr.Sum(nil)) != summary.StderrSHA256 {
			return ErrConflict
		}
	}
	return nil
}
