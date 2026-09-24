// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5"
)

// maxAgentArtifactChunks bounds the rows one artifact can spend. The wire
// contract lets a manifest close an artifact at one byte per chunk, so the
// count is bounded here rather than by MaxArtifactBytes: 256 full chunks
// carry the largest artifact, and the slack above that covers a collector
// that flushes short at the end of a file. Past it a guest is spending rows
// instead of uploading bytes.
const maxAgentArtifactChunks = 1024

// ArtifactOutcome names what an upload did to the plane's copy. Both values
// mean the plane now holds the evidence, which is what makes an agent's
// retry safe.
type ArtifactOutcome string

const (
	// ArtifactAccepted stored new bytes, or closed the artifact.
	ArtifactAccepted ArtifactOutcome = "accepted"
	// ArtifactDuplicate re-presented evidence already on file, unchanged.
	ArtifactDuplicate ArtifactOutcome = "duplicate"
)

// ArtifactOutcomes is the closed set, exported so a caller's switch and this
// package cannot fall behind each other.
func ArtifactOutcomes() []ArtifactOutcome {
	return []ArtifactOutcome{ArtifactAccepted, ArtifactDuplicate}
}

// heldArtifact is the plane's copy of one artifact under one attempt.
type heldArtifact struct {
	Exists     bool
	Closed     bool
	StepKey    string
	TotalBytes int64
	Chunks     int64
	SHA256     string
	Truncated  bool
}

// heldChunk is the stored chunk at the sequence an upload re-presents.
type heldChunk struct {
	Exists bool
	Offset int64
	SHA256 string
	Length int64
}

// artifactChunkWrite decides one chunk against what the plane holds, with no
// SQL in it. A byte-identical replay is a duplicate however often it
// arrives; anything else that is not the next contiguous chunk is refused,
// because reassembly is concatenation and a gap cannot be filled later
// without rewriting a file the plane already served.
func artifactChunkWrite(chunk protocol.ArtifactChunk, length int64, artifact heldArtifact, held heldChunk) (ArtifactOutcome, error) {
	if length < 1 || length > protocol.MaxArtifactChunkBytes {
		return "", fmt.Errorf("%w: artifact chunk length", ErrInvalid)
	}
	if artifact.Exists && artifact.StepKey != chunk.StepName {
		return "", fmt.Errorf("%w: artifact belongs to another step", ErrConflict)
	}
	if held.Exists {
		if held.SHA256 != chunk.SHA256 || held.Offset != chunk.Offset || held.Length != length {
			return "", fmt.Errorf("%w: artifact chunk altered under replay", ErrConflict)
		}
		return ArtifactDuplicate, nil
	}
	if artifact.Closed {
		return "", fmt.Errorf("%w: artifact already closed", ErrConflict)
	}
	if chunk.Sequence != artifact.Chunks+1 {
		return "", fmt.Errorf("%w: artifact chunk out of order", ErrConflict)
	}
	if chunk.Offset != artifact.TotalBytes {
		return "", fmt.Errorf("%w: artifact chunk leaves a gap", ErrConflict)
	}
	if chunk.Sequence > maxAgentArtifactChunks ||
		artifact.TotalBytes+length > protocol.MaxArtifactBytes {
		return "", fmt.Errorf("%w: artifact exceeds its bounds", ErrConflict)
	}
	return ArtifactAccepted, nil
}

// artifactClose checks a manifest against the bytes the plane actually
// stored. The manifest is evidence about an upload, never an intent, so one
// that does not describe what is on file is refused rather than believed.
// digest is the SHA-256 over the stored chunks and is only read when the
// artifact is still open.
func artifactClose(manifest protocol.ArtifactManifest, artifact heldArtifact, digest string) (ArtifactOutcome, error) {
	if !artifact.Exists {
		return "", fmt.Errorf("%w: manifest closes an artifact with no chunks", ErrConflict)
	}
	if artifact.StepKey != manifest.StepName {
		return "", fmt.Errorf("%w: manifest names another step", ErrConflict)
	}
	if artifact.Closed {
		if artifact.SHA256 != manifest.SHA256 || artifact.TotalBytes != manifest.TotalBytes ||
			artifact.Truncated != manifest.Truncated {
			return "", fmt.Errorf("%w: artifact was closed on other evidence", ErrConflict)
		}
		return ArtifactDuplicate, nil
	}
	if artifact.Chunks != manifest.FinalSequence {
		return "", fmt.Errorf("%w: manifest final sequence is not what was uploaded", ErrConflict)
	}
	if artifact.TotalBytes != manifest.TotalBytes {
		return "", fmt.Errorf("%w: manifest total is not what was uploaded", ErrConflict)
	}
	if digest != manifest.SHA256 {
		return "", fmt.Errorf("%w: manifest digest is not the uploaded bytes", ErrConflict)
	}
	return ArtifactAccepted, nil
}

// SaveAgentArtifactChunk stores one ordered slice of a step's declared output
// under the same certificate, grant, fencing and running-state rules a log
// chunk goes through.
func (s *Store) SaveAgentArtifactChunk(ctx context.Context, certDER []byte, chunk protocol.ArtifactChunk) (ArtifactOutcome, error) {
	if s == nil || s.pool == nil {
		return "", fmt.Errorf("%w: store", ErrInvalid)
	}
	if err := chunk.Validate(); err != nil {
		return "", fmt.Errorf("%w: artifact chunk", ErrInvalid)
	}
	data, err := chunk.Bytes()
	if err != nil {
		return "", fmt.Errorf("%w: artifact chunk", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return "", fmt.Errorf("%w: begin artifact chunk: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return "", err
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, chunk.AssignmentID, chunk.AttemptID,
		chunk.AssignmentVersion, chunk.FencingToken)
	if err != nil {
		return "", err
	}
	if err := agentEvidenceWindow(ctx, tx, attempt); err != nil {
		return "", err
	}
	artifact, err := lockHeldArtifact(ctx, tx, chunk.AttemptID, chunk.Path)
	if err != nil {
		return "", err
	}
	held, err := heldArtifactChunk(ctx, tx, chunk.AttemptID, chunk.Path, chunk.Sequence)
	if err != nil {
		return "", err
	}
	length := int64(len(data))
	outcome, err := artifactChunkWrite(chunk, length, artifact, held)
	if err != nil {
		return "", err
	}
	if outcome == ArtifactAccepted {
		if err := admitArtifactBytes(ctx, tx, chunk, artifact, length); err != nil {
			return "", err
		}
		_, err = tx.Exec(ctx, `INSERT INTO agent_artifact_chunks
			(attempt_id, path, seq, byte_offset, sha256, bytes)
			VALUES ($1,$2,$3,$4,$5,$6)`, chunk.AttemptID, chunk.Path,
			chunk.Sequence, chunk.Offset, chunk.SHA256, data)
		if err != nil {
			return "", fmt.Errorf("%w: insert artifact chunk: %v", ErrUnavailable, err)
		}
		// Fenced on the counters the decision was taken against, so a racing
		// upload cannot land two chunks at one position.
		tag, err := tx.Exec(ctx, `UPDATE agent_artifacts
			SET total_bytes=total_bytes+$3, chunk_count=chunk_count+1,
			updated_at=clock_timestamp()
			WHERE attempt_id=$1 AND path=$2 AND closed_at IS NULL
			AND chunk_count=$4 AND total_bytes=$5`,
			chunk.AttemptID, chunk.Path, length, artifact.Chunks, artifact.TotalBytes)
		if err != nil {
			return "", fmt.Errorf("%w: extend artifact: %v", ErrUnavailable, err)
		}
		if tag.RowsAffected() != 1 {
			return "", fmt.Errorf("%w: artifact moved under the upload", ErrConflict)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return "", fmt.Errorf("%w: commit artifact chunk: %v", ErrUnavailable, err)
	}
	return outcome, nil
}

// CloseAgentArtifact records the manifest that ends one artifact, after
// digesting the chunks on file and refusing a manifest that describes
// anything else.
func (s *Store) CloseAgentArtifact(ctx context.Context, certDER []byte, manifest protocol.ArtifactManifest) (ArtifactOutcome, error) {
	if s == nil || s.pool == nil {
		return "", fmt.Errorf("%w: store", ErrInvalid)
	}
	if err := manifest.Validate(); err != nil {
		return "", fmt.Errorf("%w: artifact manifest", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return "", fmt.Errorf("%w: begin artifact manifest: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	agent, err := agentForCertificate(ctx, tx, certDER)
	if err != nil {
		return "", err
	}
	attempt, err := lockAgentAttempt(ctx, tx, agent, manifest.AssignmentID, manifest.AttemptID,
		manifest.AssignmentVersion, manifest.FencingToken)
	if err != nil {
		return "", err
	}
	if err := agentEvidenceWindow(ctx, tx, attempt); err != nil {
		return "", err
	}
	artifact, err := lockHeldArtifact(ctx, tx, manifest.AttemptID, manifest.Path)
	if err != nil {
		return "", err
	}
	var digest string
	if artifact.Exists && !artifact.Closed {
		digest, err = storedArtifactDigest(ctx, tx, manifest.AttemptID, manifest.Path)
		if err != nil {
			return "", err
		}
	}
	outcome, err := artifactClose(manifest, artifact, digest)
	if err != nil {
		return "", err
	}
	if outcome == ArtifactAccepted {
		tag, err := tx.Exec(ctx, `UPDATE agent_artifacts
			SET sha256=$3, truncated=$4, captured_at=$5, closed_at=clock_timestamp(),
			updated_at=clock_timestamp()
			WHERE attempt_id=$1 AND path=$2 AND closed_at IS NULL
			AND chunk_count=$6 AND total_bytes=$7`,
			manifest.AttemptID, manifest.Path, manifest.SHA256, manifest.Truncated,
			manifest.CapturedAt.UTC(), artifact.Chunks, artifact.TotalBytes)
		if err != nil {
			return "", fmt.Errorf("%w: close artifact: %v", ErrUnavailable, err)
		}
		if tag.RowsAffected() != 1 {
			return "", fmt.Errorf("%w: artifact moved under the manifest", ErrConflict)
		}
		if err := appendAudit(ctx, tx, agent.Principal, "task.artifact.closed", "task",
			attempt.TaskID, "ok", "open", "closed", attempt.RunID,
			map[string]any{"attempt_id": manifest.AttemptID, "path": manifest.Path,
				"total_bytes": manifest.TotalBytes, "sha256": manifest.SHA256,
				"truncated": manifest.Truncated}); err != nil {
			return "", fmt.Errorf("%w: audit artifact: %v", ErrUnavailable, err)
		}
	}
	if err := tx.Commit(ctx); err != nil {
		return "", fmt.Errorf("%w: commit artifact manifest: %v", ErrUnavailable, err)
	}
	return outcome, nil
}

// agentEvidenceWindow is the running-state and deadline gate both uploads
// share with the log endpoint: evidence about a finished or long-expired
// attempt is refused rather than appended to a closed record.
func agentEvidenceWindow(ctx context.Context, tx pgx.Tx, attempt agentAttempt) error {
	if attempt.State != "running" {
		return ErrConflict
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&databaseNow); err != nil {
		return fmt.Errorf("%w: artifact clock: %v", ErrUnavailable, err)
	}
	if !databaseNow.Before(attempt.DeadlineAt.Add(agentEvidenceGrace)) {
		return ErrConflict
	}
	return nil
}

func lockHeldArtifact(ctx context.Context, tx pgx.Tx, attemptID, path string) (heldArtifact, error) {
	var artifact heldArtifact
	var digest *string
	var closedAt *time.Time
	err := tx.QueryRow(ctx, `SELECT step_key, total_bytes, chunk_count, sha256, truncated, closed_at
		FROM agent_artifacts WHERE attempt_id=$1 AND path=$2 FOR UPDATE`,
		attemptID, path).Scan(&artifact.StepKey, &artifact.TotalBytes, &artifact.Chunks,
		&digest, &artifact.Truncated, &closedAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return heldArtifact{}, nil
	}
	if err != nil {
		return heldArtifact{}, fmt.Errorf("%w: read artifact: %v", ErrUnavailable, err)
	}
	artifact.Exists = true
	artifact.Closed = closedAt != nil
	if digest != nil {
		artifact.SHA256 = *digest
	}
	return artifact, nil
}

func heldArtifactChunk(ctx context.Context, tx pgx.Tx, attemptID, path string, sequence int64) (heldChunk, error) {
	var chunk heldChunk
	err := tx.QueryRow(ctx, `SELECT byte_offset, sha256, octet_length(bytes)
		FROM agent_artifact_chunks WHERE attempt_id=$1 AND path=$2 AND seq=$3`,
		attemptID, path, sequence).Scan(&chunk.Offset, &chunk.SHA256, &chunk.Length)
	if errors.Is(err, pgx.ErrNoRows) {
		return heldChunk{}, nil
	}
	if err != nil {
		return heldChunk{}, fmt.Errorf("%w: read artifact chunk: %v", ErrUnavailable, err)
	}
	chunk.Exists = true
	return chunk, nil
}

// admitArtifactBytes enforces the per-attempt ceilings no single artifact can
// see, and opens the artifact row on first sight.
func admitArtifactBytes(ctx context.Context, tx pgx.Tx, chunk protocol.ArtifactChunk, artifact heldArtifact, length int64) error {
	var artifacts, total int64
	err := tx.QueryRow(ctx, `SELECT COUNT(*), COALESCE(SUM(total_bytes),0)
		FROM agent_artifacts WHERE attempt_id=$1`, chunk.AttemptID).Scan(&artifacts, &total)
	if err != nil {
		return fmt.Errorf("%w: artifact totals: %v", ErrUnavailable, err)
	}
	if total+length > protocol.MaxArtifactBytes {
		return fmt.Errorf("%w: attempt artifact budget", ErrConflict)
	}
	if artifact.Exists {
		return nil
	}
	if artifacts >= protocol.MaxArtifactsPerAttempt {
		return fmt.Errorf("%w: attempt artifact count", ErrConflict)
	}
	_, err = tx.Exec(ctx, `INSERT INTO agent_artifacts (attempt_id, path, step_key)
		VALUES ($1,$2,$3)`, chunk.AttemptID, chunk.Path, chunk.StepName)
	if err != nil {
		return fmt.Errorf("%w: open artifact: %v", ErrUnavailable, err)
	}
	return nil
}

// storedArtifactDigest digests the chunks in sequence order, which is the
// order reassembly concatenates them in.
func storedArtifactDigest(ctx context.Context, tx pgx.Tx, attemptID, path string) (string, error) {
	rows, err := tx.Query(ctx, `SELECT bytes FROM agent_artifact_chunks
		WHERE attempt_id=$1 AND path=$2 ORDER BY seq`, attemptID, path)
	if err != nil {
		return "", fmt.Errorf("%w: read artifact bytes: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	sum := sha256.New()
	for rows.Next() {
		var data []byte
		if err := rows.Scan(&data); err != nil {
			return "", fmt.Errorf("%w: scan artifact bytes: %v", ErrUnavailable, err)
		}
		sum.Write(data)
	}
	if err := rows.Err(); err != nil {
		return "", fmt.Errorf("%w: stream artifact bytes: %v", ErrUnavailable, err)
	}
	return hex.EncodeToString(sum.Sum(nil)), nil
}
