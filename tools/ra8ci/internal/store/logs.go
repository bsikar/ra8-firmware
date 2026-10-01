// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
)

// MaxLogPageSize keeps each base64-encoded API response below the JSON body cap.
const MaxLogPageSize = 8

// LogRecord is one digest-verified output chunk from an attempt.
type LogRecord struct {
	Sequence          int64  `json:"sequence"`
	Stream            string `json:"stream"`
	StepName          string `json:"step_name"`
	MonotonicOffsetNS int64  `json:"monotonic_offset_ns"`
	SHA256            string `json:"sha256"`
	DataBase64        string `json:"data_base64"`
	Data              []byte `json:"-"`
}

// LogPage is a globally ordered, cursor-paginated attempt log response.
type LogPage struct {
	AttemptID string      `json:"attempt_id"`
	Chunks    []LogRecord `json:"chunks"`
	NextAfter int64       `json:"next_after"`
	HasMore   bool        `json:"has_more"`
}

// AttemptLogs returns digest-checked log chunks for an attempt belonging to
// the requested run. The page size bounds response memory and JSON encoding.
func (s *Store) AttemptLogs(ctx context.Context, runID, attemptID string, after int64, limit int) (LogPage, error) {
	if s == nil || s.pool == nil {
		return LogPage{}, ErrUnavailable
	}
	if !ValidID(runID) || !ValidID(attemptID) || after < 0 || limit < 1 || limit > MaxLogPageSize {
		return LogPage{}, fmt.Errorf("%w: log page parameters", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT c.seq, c.stream, COALESCE(c.agent_step_key, c.step_key, ''), c.monotonic_offset_ns, c.sha256, c.bytes
		FROM log_chunks c
		JOIN task_attempts a ON a.id=c.attempt_id
		JOIN tasks t ON t.id=a.task_id
		WHERE t.run_id=$1 AND c.attempt_id=$2 AND c.seq>$3
		ORDER BY c.seq LIMIT $4`, runID, attemptID, after, limit+1)
	if err != nil {
		return LogPage{}, fmt.Errorf("%w: query attempt logs: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	page := LogPage{AttemptID: attemptID, Chunks: make([]LogRecord, 0, limit), NextAfter: after}
	for rows.Next() {
		var record LogRecord
		var data []byte
		if err := rows.Scan(&record.Sequence, &record.Stream, &record.StepName, &record.MonotonicOffsetNS, &record.SHA256, &data); err != nil {
			return LogPage{}, fmt.Errorf("%w: scan attempt log: %v", ErrUnavailable, err)
		}
		if len(page.Chunks) == limit {
			page.HasMore = true
			break
		}
		if record.Sequence != page.NextAfter+1 || (record.Stream != "stdout" && record.Stream != "stderr") ||
			record.MonotonicOffsetNS < 0 || len(data) == 0 || len(data) > 65536 {
			return LogPage{}, fmt.Errorf("%w: invalid stored log chunk", ErrUnavailable)
		}
		sum := sha256.Sum256(data)
		if hex.EncodeToString(sum[:]) != record.SHA256 {
			return LogPage{}, fmt.Errorf("%w: stored log digest mismatch", ErrUnavailable)
		}
		record.DataBase64 = base64.StdEncoding.EncodeToString(data)
		page.Chunks = append(page.Chunks, record)
		page.NextAfter = record.Sequence
	}
	if err := rows.Err(); err != nil {
		return LogPage{}, fmt.Errorf("%w: attempt log rows: %v", ErrUnavailable, err)
	}
	return page, nil
}
