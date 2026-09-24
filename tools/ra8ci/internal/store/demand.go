// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"

	"github.com/jackc/pgx/v5"
)

// DemandOutcome is what recording one delivery did to the unit of demand it
// described. The caller is a webhook endpoint that has to answer GitHub, so
// every outcome here is a 2xx: none of them is a delivery worth retrying.
type DemandOutcome string

const (
	// DemandAccepted is the first sight of this unit of demand.
	DemandAccepted DemandOutcome = "accepted"
	// DemandSuperseded is a later phase replacing what was held.
	DemandSuperseded DemandOutcome = "superseded"
	// DemandStale is a delivery that arrived after a later phase already
	// did, or the same phase a second time: the row is left alone.
	DemandStale DemandOutcome = "stale"
	// DemandDuplicate is a delivery id already recorded for this unit of
	// demand. GitHub retries deliveries, so this is expected traffic.
	DemandDuplicate DemandOutcome = "duplicate"
)

// DemandRecord is a stored unit of demand: the event, plus when it was first
// seen and how many times the row has moved.
type DemandRecord struct {
	Event       demand.Event `json:"event"`
	FirstSeenAt time.Time    `json:"first_seen_at"`
	UpdatedAt   time.Time    `json:"updated_at"`
	Version     int64        `json:"version"`
}

// demandWrite is the decision a delivery implies about the row already held,
// separated from the SQL so the ordering rule can be tested without a
// database. It defers to the demand package rather than restating the phase
// ranking: one place decides what supersedes what.
func demandWrite(incoming demand.Event, held demand.Event, exists bool) DemandOutcome {
	if !exists {
		return DemandAccepted
	}
	if incoming.Supersedes(held) {
		return DemandSuperseded
	}
	return DemandStale
}

func demandLabels(labels []string) (json.RawMessage, error) {
	encoded, err := json.Marshal(labels)
	if err != nil {
		return nil, fmt.Errorf("%w: labels: %v", ErrInvalid, err)
	}
	return encoded, nil
}

func nullableTime(value time.Time) any {
	if value.IsZero() {
		return nil
	}
	return value
}

func nullableText(value string) any {
	if value == "" {
		return nil
	}
	return value
}

// RecordDemandEvent stores one normalized demand event, keyed on the unit of
// demand rather than on the delivery that carried it. A replayed delivery,
// two adapters describing the same job, and a queued delivery that lands
// after the completed one all converge on the same row: the identity is the
// job and run attempt, and only a strictly later phase moves it.
func (s *Store) RecordDemandEvent(ctx context.Context, event demand.Event) (DemandOutcome, error) {
	if err := event.Validate(); err != nil {
		return "", fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	labels, err := demandLabels(event.Labels)
	if err != nil {
		return "", err
	}
	key := event.Key()
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return "", fmt.Errorf("%w: begin demand write: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()

	// Insert first so the row exists for the delivery's foreign key, and
	// take the lock in the same statement when the row is already there.
	var inserted string
	err = tx.QueryRow(ctx, `INSERT INTO demand_events
		(demand_key, job_id, run_id, run_attempt, phase, adapter, delivery_id, owner,
		 repository, workflow, job_name, commit_sha, labels, runner_name, conclusion,
		 queued_at, started_at, completed_at, observed_at)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,$19)
		ON CONFLICT (demand_key) DO NOTHING RETURNING demand_key`,
		key, event.JobID, event.RunID, event.RunAttempt, string(event.Phase), event.Adapter,
		event.DeliveryID, event.Owner, event.Repository, event.Workflow, event.JobName,
		event.CommitSHA, labels, nullableText(event.RunnerName), nullableText(event.Conclusion),
		event.QueuedAt, nullableTime(event.StartedAt), nullableTime(event.CompletedAt),
		event.ObservedAt).Scan(&inserted)
	outcome := DemandAccepted
	switch {
	case errors.Is(err, pgx.ErrNoRows):
		outcome, err = s.moveDemandRow(ctx, tx, key, event, labels)
		if err != nil {
			return "", err
		}
	case err != nil:
		return "", fmt.Errorf("%w: demand insert: %v", ErrUnavailable, err)
	}

	duplicate, err := recordDemandDelivery(ctx, tx, key, event)
	if err != nil {
		return "", err
	}
	if duplicate {
		// The row was already at this delivery's phase, so the write
		// above was a no-op; report the delivery for what it is.
		outcome = DemandDuplicate
	} else if err := appendAudit(ctx, tx, "demand:"+event.Adapter, "demand.event."+string(outcome),
		"demand", key, "ok", "", string(event.Phase), "", map[string]any{
			"delivery_id": event.DeliveryID, "repository": event.FullRepository()}); err != nil {
		return "", fmt.Errorf("%w: demand audit: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return "", fmt.Errorf("%w: commit demand write: %v", ErrUnavailable, err)
	}
	return outcome, nil
}

// moveDemandRow decides and applies what an incoming event does to the row
// already held for the same unit of demand.
func (s *Store) moveDemandRow(ctx context.Context, tx pgx.Tx, key string,
	event demand.Event, labels json.RawMessage) (DemandOutcome, error) {
	var heldPhase string
	err := tx.QueryRow(ctx, `SELECT phase FROM demand_events WHERE demand_key=$1 FOR UPDATE`,
		key).Scan(&heldPhase)
	if err != nil {
		return "", fmt.Errorf("%w: lock demand row: %v", ErrUnavailable, err)
	}
	held := demand.Event{JobID: event.JobID, RunAttempt: event.RunAttempt, Phase: demand.Phase(heldPhase)}
	outcome := demandWrite(event, held, true)
	if outcome != DemandSuperseded {
		return outcome, nil
	}
	tag, err := tx.Exec(ctx, `UPDATE demand_events SET phase=$2, adapter=$3, delivery_id=$4,
		workflow=$5, job_name=$6, labels=$7, runner_name=$8, conclusion=$9,
		started_at=$10, completed_at=$11, observed_at=$12,
		updated_at=clock_timestamp(), version=version+1
		WHERE demand_key=$1 AND phase=$13`,
		key, string(event.Phase), event.Adapter, event.DeliveryID, event.Workflow, event.JobName,
		labels, nullableText(event.RunnerName), nullableText(event.Conclusion),
		nullableTime(event.StartedAt), nullableTime(event.CompletedAt), event.ObservedAt, heldPhase)
	if err != nil {
		return "", fmt.Errorf("%w: demand update: %v", ErrUnavailable, err)
	}
	if tag.RowsAffected() != 1 {
		return "", fmt.Errorf("%w: demand row moved under the delivery", ErrConflict)
	}
	return outcome, nil
}

// recordDemandDelivery writes the delivery that carried this event and
// reports whether it had already been recorded. A delivery id reused for a
// different unit of demand is a conflict, not a duplicate: the two ids are
// not describing the same thing and one of them is wrong.
func recordDemandDelivery(ctx context.Context, tx pgx.Tx, key string, event demand.Event) (bool, error) {
	var recorded string
	err := tx.QueryRow(ctx, `INSERT INTO demand_deliveries
		(adapter, delivery_id, demand_key, phase, observed_at) VALUES ($1,$2,$3,$4,$5)
		ON CONFLICT DO NOTHING RETURNING demand_key`,
		event.Adapter, event.DeliveryID, key, string(event.Phase), event.ObservedAt).Scan(&recorded)
	if err == nil {
		return false, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return false, fmt.Errorf("%w: delivery insert: %v", ErrUnavailable, err)
	}
	var heldKey, heldPhase string
	if err := tx.QueryRow(ctx, `SELECT demand_key, phase FROM demand_deliveries
		WHERE adapter=$1 AND delivery_id=$2`, event.Adapter, event.DeliveryID).
		Scan(&heldKey, &heldPhase); err != nil {
		return false, fmt.Errorf("%w: delivery lookup: %v", ErrUnavailable, err)
	}
	if heldKey != key || heldPhase != string(event.Phase) {
		return false, fmt.Errorf("%w: delivery %q already describes %s at %s",
			ErrConflict, event.DeliveryID, heldKey, heldPhase)
	}
	return true, nil
}

// GetDemandEvent reads the current state of one unit of demand.
func (s *Store) GetDemandEvent(ctx context.Context, key string) (DemandRecord, error) {
	var record DemandRecord
	var labels []byte
	var runnerName, conclusion *string
	var startedAt, completedAt *time.Time
	err := s.pool.QueryRow(ctx, `SELECT job_id, run_id, run_attempt, phase, adapter, delivery_id,
		owner, repository, workflow, job_name, commit_sha, labels, runner_name, conclusion,
		queued_at, started_at, completed_at, observed_at, first_seen_at, updated_at, version
		FROM demand_events WHERE demand_key=$1`, key).Scan(
		&record.Event.JobID, &record.Event.RunID, &record.Event.RunAttempt, &record.Event.Phase,
		&record.Event.Adapter, &record.Event.DeliveryID, &record.Event.Owner, &record.Event.Repository,
		&record.Event.Workflow, &record.Event.JobName, &record.Event.CommitSHA, &labels,
		&runnerName, &conclusion, &record.Event.QueuedAt, &startedAt, &completedAt,
		&record.Event.ObservedAt, &record.FirstSeenAt, &record.UpdatedAt, &record.Version)
	if errors.Is(err, pgx.ErrNoRows) {
		return DemandRecord{}, ErrNotFound
	}
	if err != nil {
		return DemandRecord{}, fmt.Errorf("%w: demand lookup: %v", ErrUnavailable, err)
	}
	if err := json.Unmarshal(labels, &record.Event.Labels); err != nil {
		return DemandRecord{}, fmt.Errorf("%w: stored labels: %v", ErrUnavailable, err)
	}
	if runnerName != nil {
		record.Event.RunnerName = *runnerName
	}
	if conclusion != nil {
		record.Event.Conclusion = *conclusion
	}
	if startedAt != nil {
		record.Event.StartedAt = *startedAt
	}
	if completedAt != nil {
		record.Event.CompletedAt = *completedAt
	}
	return record, nil
}

// ListOpenDemand reads the demand that has not reached a conclusion, oldest
// queue time first. The reconciliation pass uses it to ask GitHub about work
// whose completion delivery may simply never have arrived.
func (s *Store) ListOpenDemand(ctx context.Context, limit int) ([]DemandRecord, error) {
	if limit < 1 || limit > 1000 {
		return nil, fmt.Errorf("%w: demand page size", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT demand_key FROM demand_events
		WHERE phase <> 'completed' ORDER BY queued_at, demand_key LIMIT $1`, limit)
	if err != nil {
		return nil, fmt.Errorf("%w: open demand query: %v", ErrUnavailable, err)
	}
	keys := make([]string, 0, limit)
	for rows.Next() {
		var key string
		if err := rows.Scan(&key); err != nil {
			rows.Close()
			return nil, fmt.Errorf("%w: open demand scan: %v", ErrUnavailable, err)
		}
		keys = append(keys, key)
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: open demand rows: %v", ErrUnavailable, err)
	}
	records := make([]DemandRecord, 0, len(keys))
	for _, key := range keys {
		record, err := s.GetDemandEvent(ctx, key)
		if err != nil {
			return nil, err
		}
		records = append(records, record)
	}
	return records, nil
}
