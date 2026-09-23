package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

var localIDPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// LocalRunInput is a report of a task already run on the caller's own host.
// It is never admitted to the dispatch queue or asserted as a CI pass.
// PrincipalID and PayloadSHA256 must come from the authenticated HTTP layer.
type LocalRunInput struct {
	PrincipalID        string
	LocalID            string
	PayloadSHA256      string
	SourceVerification string
	Repository         string
	Branch             string
	CommitSHA          string
	SnapshotSHA256     string
	CatalogSHA256      string
	TaskName           string
	Tier               string
	Scope              string
	DeadlineSeconds    int
	Arguments          []string
	StartedAt          time.Time
	FinishedAt         time.Time
	DurationNS         int64
	Result             string
	ChildExitCode      int
	ExecutorError      string
	Steps              []LocalStepInput
}

type LocalStepInput struct {
	Key          string
	Ordinal      int
	StartedAt    time.Time
	EndedAt      time.Time
	DurationNS   int64
	ExitCode     int
	TimedOut     bool
	Cancelled    bool
	StdoutSHA256 string
	StderrSHA256 string
	StdoutBytes  int64
	StderrBytes  int64
}

// LocalRunReceipt is a durable database mapping; the client may mark its
// outbox record synced only after receiving this exact committed receipt.
type LocalRunReceipt struct {
	LocalRunID    string `json:"local_run_id"`
	LocalID       string `json:"local_id"`
	PayloadSHA256 string `json:"payload_sha256"`
}

// LookupLocalRunReceipt resolves a previously committed upload independent of
// the current task catalog. This lets a client recover a lost HTTP response
// after a server upgrade, without reinterpreting historical evidence.
func (s *Store) LookupLocalRunReceipt(ctx context.Context, principalID, localID, payloadSHA256 string) (LocalRunReceipt, error) {
	if s == nil || s.pool == nil {
		return LocalRunReceipt{}, ErrUnavailable
	}
	if principalID == "" || len(principalID) > 256 || !localIDPattern.MatchString(localID) ||
		!hexSHA.MatchString(payloadSHA256) {
		return LocalRunReceipt{}, ErrInvalid
	}
	var receipt LocalRunReceipt
	err := s.pool.QueryRow(ctx, `SELECT id::text,local_id,payload_sha256 FROM local_runs
		WHERE principal_id=$1 AND local_id=$2`, principalID, localID).
		Scan(&receipt.LocalRunID, &receipt.LocalID, &receipt.PayloadSHA256)
	if errors.Is(err, pgx.ErrNoRows) {
		return LocalRunReceipt{}, ErrNotFound
	}
	if err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: local receipt lookup: %v", ErrUnavailable, err)
	}
	if receipt.PayloadSHA256 != payloadSHA256 {
		return LocalRunReceipt{}, fmt.Errorf("%w: local ID reused with different evidence", ErrConflict)
	}
	return receipt, nil
}

// IngestLocalRun atomically records local history and a permanent idempotency
// receipt. A retry with identical evidence returns the original receipt; the
// same principal/local ID with changed bytes is a conflict, even years later.
func (s *Store) IngestLocalRun(ctx context.Context, in LocalRunInput) (LocalRunReceipt, error) {
	if s == nil || s.pool == nil {
		return LocalRunReceipt{}, ErrUnavailable
	}
	if err := validateLocalRun(in); err != nil {
		return LocalRunReceipt{}, err
	}
	arguments := in.Arguments
	if arguments == nil {
		arguments = []string{}
	}
	args, err := json.Marshal(arguments)
	if err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: local arguments: %v", ErrInvalid, err)
	}
	// The advisory lock serializes retries; READ COMMITTED lets a waiter see
	// the winner's committed row after it acquires the lock.
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.ReadCommitted})
	if err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: begin local ingest: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	lockKey := fmt.Sprintf("local:%d:%s:%s", len(in.PrincipalID), in.PrincipalID, in.LocalID)
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 8))", lockKey); err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: local ingest lock: %v", ErrUnavailable, err)
	}
	var oldID, oldHash string
	err = tx.QueryRow(ctx, `SELECT id::text,payload_sha256 FROM local_runs
		WHERE principal_id=$1 AND local_id=$2`, in.PrincipalID, in.LocalID).Scan(&oldID, &oldHash)
	if err == nil {
		outcome, action := "replayed", "local_run.replayed"
		if oldHash != in.PayloadSHA256 {
			outcome, action = "denied", "local_run.conflict"
		}
		if auditErr := appendAudit(ctx, tx, in.PrincipalID, action, "local_run", oldID,
			outcome, "", "", "", map[string]any{"local_id": in.LocalID,
				"payload_sha256": in.PayloadSHA256}); auditErr != nil {
			return LocalRunReceipt{}, fmt.Errorf("%w: audit local replay: %v", ErrUnavailable, auditErr)
		}
		if commitErr := tx.Commit(ctx); commitErr != nil {
			return LocalRunReceipt{}, fmt.Errorf("%w: commit local replay: %v", ErrUnavailable, commitErr)
		}
		if oldHash != in.PayloadSHA256 {
			return LocalRunReceipt{}, fmt.Errorf("%w: local ID was reused with different evidence", ErrConflict)
		}
		return LocalRunReceipt{LocalRunID: oldID, LocalID: in.LocalID, PayloadSHA256: oldHash}, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return LocalRunReceipt{}, fmt.Errorf("%w: local replay lookup: %v", ErrUnavailable, err)
	}
	id, err := NewID()
	if err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: local run ID: %v", ErrUnavailable, err)
	}
	_, err = tx.Exec(ctx, `INSERT INTO local_runs
		(id,principal_id,local_id,payload_sha256,source_verification,repository,branch,
		commit_sha,snapshot_sha256,catalog_sha256,task_name,tier,scope,deadline_seconds,
		arguments,started_at,finished_at,duration_ns,result,child_exit_code,executor_error)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19,$20,$21)`,
		id, in.PrincipalID, in.LocalID, in.PayloadSHA256, in.SourceVerification,
		in.Repository, in.Branch, nullable(in.CommitSHA), nullable(in.SnapshotSHA256),
		in.CatalogSHA256, in.TaskName, in.Tier, in.Scope, in.DeadlineSeconds,
		args, in.StartedAt, in.FinishedAt, in.DurationNS, in.Result, in.ChildExitCode, in.ExecutorError)
	if err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: insert local run: %v", ErrUnavailable, err)
	}
	for _, step := range in.Steps {
		_, err = tx.Exec(ctx, `INSERT INTO local_run_steps
			(local_run_id,ordinal,step_key,started_at,ended_at,duration_ns,child_exit_code,
			timed_out,cancelled,stdout_sha256,stderr_sha256,stdout_bytes,stderr_bytes)
			VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)`,
			id, step.Ordinal, step.Key, step.StartedAt, step.EndedAt, step.DurationNS,
			step.ExitCode, step.TimedOut, step.Cancelled, step.StdoutSHA256, step.StderrSHA256,
			step.StdoutBytes, step.StderrBytes)
		if err != nil {
			return LocalRunReceipt{}, fmt.Errorf("%w: insert local step %q: %v", ErrUnavailable, step.Key, err)
		}
	}
	if err := appendAudit(ctx, tx, in.PrincipalID, "local_run.ingested", "local_run", id,
		"reported", "", in.Result, "", map[string]any{"local_id": in.LocalID,
			"payload_sha256": in.PayloadSHA256, "source_verification": in.SourceVerification,
			"step_count": len(in.Steps)}); err != nil {
		return LocalRunReceipt{}, fmt.Errorf("%w: audit local ingest: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		if isSerializationFailure(err) {
			return LocalRunReceipt{}, fmt.Errorf("%w: concurrent local ingest: %v", ErrConflict, err)
		}
		return LocalRunReceipt{}, fmt.Errorf("%w: commit local ingest: %v", ErrUnavailable, err)
	}
	return LocalRunReceipt{LocalRunID: id, LocalID: in.LocalID, PayloadSHA256: in.PayloadSHA256}, nil
}

func validateLocalRun(in LocalRunInput) error {
	if len(in.PrincipalID) == 0 || len(in.PrincipalID) > 256 || !localIDPattern.MatchString(in.LocalID) ||
		!hexSHA.MatchString(in.PayloadSHA256) || !hexSHA.MatchString(in.CatalogSHA256) ||
		len(in.Repository) == 0 || len(in.Repository) > 512 || len(in.Branch) > 512 ||
		in.SourceVerification != "verified" && in.SourceVerification != "unverified" ||
		in.CommitSHA != "" && !commitSHA.MatchString(in.CommitSHA) ||
		in.SnapshotSHA256 != "" && !hexSHA.MatchString(in.SnapshotSHA256) ||
		in.SourceVerification == "verified" && (in.CommitSHA == "" || in.SnapshotSHA256 == "") ||
		in.SourceVerification == "unverified" && in.SnapshotSHA256 != "" ||
		len(in.TaskName) == 0 || len(in.TaskName) > 128 || strings.TrimSpace(in.TaskName) != in.TaskName ||
		in.Tier != "required" && in.Tier != "optional" && in.Tier != "nightly" ||
		in.Scope != "safe-local-read-only" && in.Scope != "safe-local-write-working-tree" ||
		in.DeadlineSeconds < 1 || in.DeadlineSeconds > 86400 || len(in.Arguments) > 64 ||
		in.StartedAt.IsZero() || in.FinishedAt.IsZero() || in.FinishedAt.Before(in.StartedAt) ||
		in.DurationNS < 0 || len(in.ExecutorError) > 1024 || len(in.Steps) > 128 {
		return fmt.Errorf("%w: invalid local run metadata", ErrInvalid)
	}
	switch in.Result {
	case "succeeded":
		if in.ChildExitCode != 0 || in.ExecutorError != "" {
			return fmt.Errorf("%w: contradictory local success", ErrInvalid)
		}
	case "failed", "timed_out", "cancelled", "incomplete_evidence":
	default:
		return fmt.Errorf("%w: local result", ErrInvalid)
	}
	seen := make(map[string]bool, len(in.Steps))
	for i, step := range in.Steps {
		if step.Ordinal != i || len(step.Key) == 0 || len(step.Key) > 128 || seen[step.Key] ||
			step.StartedAt.IsZero() || step.EndedAt.IsZero() || step.EndedAt.Before(step.StartedAt) ||
			step.StartedAt.Before(in.StartedAt) || step.EndedAt.After(in.FinishedAt) ||
			step.DurationNS < 0 || step.TimedOut && step.Cancelled ||
			!hexSHA.MatchString(step.StdoutSHA256) || !hexSHA.MatchString(step.StderrSHA256) ||
			step.StdoutBytes < 0 || step.StderrBytes < 0 {
			return fmt.Errorf("%w: invalid local step %d", ErrInvalid, i)
		}
		seen[step.Key] = true
	}
	return nil
}
