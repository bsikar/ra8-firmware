// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"fmt"
	"time"
)

const (
	MaxEventPageSize = 50
	maxRunEventBytes = 8 << 10
)

// RunEvent is an immutable state-transition record with bounded JSON details.
type RunEvent struct {
	Sequence   int64           `json:"sequence"`
	ID         string          `json:"id"`
	Kind       string          `json:"kind"`
	Data       json.RawMessage `json:"data"`
	HappenedAt time.Time       `json:"happened_at"`
}

// RunEventPage is a bounded contiguous slice of the append-only run event log.
type RunEventPage struct {
	RunID     string     `json:"run_id"`
	Events    []RunEvent `json:"events"`
	NextAfter int64      `json:"next_after"`
	HasMore   bool       `json:"has_more"`
}

// RunEvents reads a contiguous cursor page without exposing another run's events.
func (s *Store) RunEvents(ctx context.Context, runID string, after int64, limit int) (RunEventPage, error) {
	if s == nil || s.pool == nil {
		return RunEventPage{}, ErrUnavailable
	}
	if !ValidID(runID) || after < 0 || limit < 1 || limit > MaxEventPageSize {
		return RunEventPage{}, fmt.Errorf("%w: run event page parameters", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT event_seq, id::text, kind, data::text, happened_at
		FROM run_events WHERE run_id=$1 AND event_seq>$2 ORDER BY event_seq LIMIT $3`,
		runID, after, limit+1)
	if err != nil {
		return RunEventPage{}, fmt.Errorf("%w: query run events: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	page := RunEventPage{RunID: runID, Events: make([]RunEvent, 0, limit), NextAfter: after}
	for rows.Next() {
		var event RunEvent
		var raw string
		if err := rows.Scan(&event.Sequence, &event.ID, &event.Kind, &raw, &event.HappenedAt); err != nil {
			return RunEventPage{}, fmt.Errorf("%w: scan run event: %v", ErrUnavailable, err)
		}
		if len(page.Events) == limit {
			page.HasMore = true
			break
		}
		var object map[string]json.RawMessage
		if event.Sequence != page.NextAfter+1 || !ValidID(event.ID) || len(event.Kind) < 1 || len(event.Kind) > 128 ||
			event.HappenedAt.IsZero() || len(raw) < 2 || len(raw) > maxRunEventBytes ||
			json.Unmarshal([]byte(raw), &object) != nil || object == nil {
			return RunEventPage{}, fmt.Errorf("%w: invalid stored run event", ErrUnavailable)
		}
		event.Data = json.RawMessage(raw)
		page.Events = append(page.Events, event)
		page.NextAfter = event.Sequence
	}
	if err := rows.Err(); err != nil {
		return RunEventPage{}, fmt.Errorf("%w: run event rows: %v", ErrUnavailable, err)
	}
	return page, nil
}
