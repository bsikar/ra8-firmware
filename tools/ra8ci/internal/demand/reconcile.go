// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"time"
)

// ReconcileAdapter names the events this package synthesizes from a poll
// rather than from a delivery. It is evidence about how the copy arrived, so
// it never takes part in identity: a reconciled completion and the webhook
// completion that was dropped describe the same unit of demand and land on
// the same row.
const ReconcileAdapter = "actions-jobs-api"

// maxReconcileFailures bounds how many source failures one pass will absorb
// before it gives up. A forge that is refusing every request is a condition
// to report, not a list to walk to the end.
const maxReconcileFailures = 8

// JobSnapshot is what the jobs API says about one job right now. It carries
// only the fields a poll can move: identity comes from the demand already on
// file, because the poll is asked about a specific job id.
type JobSnapshot struct {
	Phase       Phase
	Conclusion  string
	RunnerName  string
	StartedAt   time.Time
	CompletedAt time.Time
}

// JobSource reads the current state of one job. The Actions jobs API is the
// production implementation; a mocked adapter is the tested one. A job the
// forge no longer knows about is a normal false result, not an error.
type JobSource interface {
	Job(ctx context.Context, owner, repository string, jobID int64) (JobSnapshot, bool, error)
}

// OpenDemandStore is the persistence side of the pass. The method names are
// deliberately not the store's own, so a type has to opt in to this contract
// rather than satisfy it by accident.
type OpenDemandStore interface {
	ListOpen(ctx context.Context, limit int) ([]Event, error)
	Record(ctx context.Context, event Event) error
}

// ReconcilerConfig configures one reconciliation pass.
type ReconcilerConfig struct {
	Source JobSource
	Store  OpenDemandStore
	// Adapter defaults to ReconcileAdapter.
	Adapter string
	// Grace is how long after the last observation a unit of demand is
	// left alone. A delivery in flight is the common case; polling inside
	// the grace window spends API budget racing the webhook.
	Grace time.Duration
	// MissingAfter is how long demand the forge no longer knows about
	// stays open before the pass concludes it. Below it, a job missing
	// from the API is treated as not yet visible rather than gone.
	MissingAfter time.Duration
	// BatchSize bounds the units of demand one pass reads.
	BatchSize int
	// Now defaults to time.Now.
	Now func() time.Time
}

// Report is what one pass did. Every counter names a decision, so a pass
// that changed nothing still says why.
type Report struct {
	// Scanned is the open demand the pass read.
	Scanned int
	// Waiting is demand left alone inside the grace window, or missing
	// from the forge for less than MissingAfter.
	Waiting int
	// Advanced is demand the poll moved to a later phase.
	Advanced int
	// Unchanged is demand the forge agrees is still where it was.
	Unchanged int
	// Concluded is demand the forge no longer knows about, recorded as
	// completed/stale so it stops being open.
	Concluded int
	// Failed is demand the pass could not settle: a source error, or a
	// snapshot that does not describe a state this plane can record.
	Failed int
}

// Reconciler turns a dropped delivery into a late run. GitHub delivers
// workflow_job at least once, which also means at most zero: a completion
// that never arrives leaves demand open forever unless something asks.
type Reconciler struct {
	source       JobSource
	store        OpenDemandStore
	adapter      string
	grace        time.Duration
	missingAfter time.Duration
	batchSize    int
	now          func() time.Time
}

// NewReconciler validates the configuration up front so a pass cannot run
// half-configured against a live forge.
func NewReconciler(config ReconcilerConfig) (*Reconciler, error) {
	adapter := config.Adapter
	if adapter == "" {
		adapter = ReconcileAdapter
	}
	now := config.Now
	if now == nil {
		now = time.Now
	}
	switch {
	case config.Source == nil || config.Store == nil:
		return nil, errors.New("reconciler requires a job source and a demand store")
	case len(adapter) > 64:
		return nil, errors.New("reconciler adapter name is too long")
	case config.Grace < 0 || config.Grace > time.Hour:
		return nil, errors.New("reconciler grace must be between zero and one hour")
	case config.MissingAfter < config.Grace || config.MissingAfter > 24*time.Hour:
		return nil, errors.New("reconciler missing-after must be at least the grace and at most a day")
	case config.BatchSize < 1 || config.BatchSize > 1000:
		return nil, errors.New("reconciler batch size must be between one and a thousand")
	}
	return &Reconciler{source: config.Source, store: config.Store, adapter: adapter,
		grace: config.Grace, missingAfter: config.MissingAfter,
		batchSize: config.BatchSize, now: now}, nil
}

// reconcileDeliveryID is the delivery id a poll records. It is stable for a
// unit of demand at a phase, so repeating the pass records the same evidence
// twice instead of inventing a new delivery each time, and a poll that sees a
// later phase records a distinct one.
func reconcileDeliveryID(key string, phase Phase) string {
	return "reconcile." + replaceKeySeparator(key) + "." + string(phase)
}

func replaceKeySeparator(key string) string {
	out := []byte(key)
	for i, b := range out {
		if b == '/' {
			out[i] = '.'
		}
	}
	return string(out)
}

// observed merges a snapshot onto the demand already on file. Identity,
// repository and queue time come from the held event because the poll asked
// about that job; only what a poll can legitimately learn is taken from the
// snapshot.
func (e Event) observed(adapter string, snapshot JobSnapshot, observedAt time.Time) (Event, error) {
	if !snapshot.Phase.valid() {
		return Event{}, fmt.Errorf("%w: snapshot phase %q", ErrInvalid, snapshot.Phase)
	}
	next := e
	next.Adapter = adapter
	next.DeliveryID = reconcileDeliveryID(e.Key(), snapshot.Phase)
	next.Phase = snapshot.Phase
	next.Conclusion = snapshot.Conclusion
	next.StartedAt = snapshot.StartedAt
	next.CompletedAt = snapshot.CompletedAt
	next.ObservedAt = observedAt.UTC()
	if snapshot.RunnerName != "" {
		next.RunnerName = snapshot.RunnerName
	}
	if err := next.Validate(); err != nil {
		return Event{}, err
	}
	return next, nil
}

// concluded is the event for demand the forge no longer knows about. The
// conclusion is stale, which is what it is: the control plane never saw the
// job end and the forge can no longer say how it did.
func (e Event) concluded(adapter string, at time.Time) (Event, error) {
	next := e
	next.Adapter = adapter
	next.Phase = PhaseCompleted
	next.DeliveryID = reconcileDeliveryID(e.Key(), PhaseCompleted)
	next.Conclusion = "stale"
	next.ObservedAt = at.UTC()
	next.CompletedAt = at.UTC()
	if next.StartedAt.IsZero() {
		next.StartedAt = e.QueuedAt
	}
	if next.CompletedAt.Before(next.QueuedAt) {
		next.CompletedAt = next.QueuedAt
	}
	if err := next.Validate(); err != nil {
		return Event{}, err
	}
	return next, nil
}

// Pass reads the open demand once and asks the forge about anything that has
// been quiet for longer than the grace window. A source failure on one unit
// of demand is counted and skipped rather than abandoning the rest; only a
// store failure, or a source that keeps failing, ends the pass.
func (r *Reconciler) Pass(ctx context.Context) (Report, error) {
	if r == nil || ctx == nil {
		return Report{}, errors.New("invalid reconciliation pass")
	}
	open, err := r.store.ListOpen(ctx, r.batchSize)
	if err != nil {
		return Report{}, fmt.Errorf("list open demand: %w", err)
	}
	report := Report{Scanned: len(open)}
	var firstFailure error
	for _, held := range open {
		now := r.now().UTC()
		if now.Sub(held.ObservedAt) < r.grace {
			report.Waiting++
			continue
		}
		snapshot, found, err := r.source.Job(ctx, held.Owner, held.Repository, held.JobID)
		if err != nil {
			report.Failed++
			if firstFailure == nil {
				firstFailure = fmt.Errorf("job %s: %w", held.Key(), err)
			}
			if report.Failed >= maxReconcileFailures {
				return report, fmt.Errorf("reconciliation abandoned after %d source failures: %w",
					report.Failed, firstFailure)
			}
			continue
		}
		next, decision, err := r.decide(held, snapshot, found, now)
		if err != nil {
			report.Failed++
			if firstFailure == nil {
				firstFailure = fmt.Errorf("job %s: %w", held.Key(), err)
			}
			continue
		}
		switch decision {
		case decisionWaiting:
			report.Waiting++
			continue
		case decisionUnchanged:
			report.Unchanged++
			continue
		}
		if err := r.store.Record(ctx, next); err != nil {
			return report, fmt.Errorf("record reconciled demand %s: %w", held.Key(), err)
		}
		if decision == decisionConcluded {
			report.Concluded++
		} else {
			report.Advanced++
		}
	}
	return report, firstFailure
}

type decision int

const (
	decisionWaiting decision = iota
	decisionUnchanged
	decisionAdvanced
	decisionConcluded
)

// decide is the pass's judgement with no I/O in it: given what is held, what
// the forge says, and the time, what should be written.
func (r *Reconciler) decide(held Event, snapshot JobSnapshot, found bool, now time.Time) (Event, decision, error) {
	if !found {
		if now.Sub(held.QueuedAt) < r.missingAfter {
			return Event{}, decisionWaiting, nil
		}
		next, err := held.concluded(r.adapter, now)
		if err != nil {
			return Event{}, decisionWaiting, err
		}
		return next, decisionConcluded, nil
	}
	next, err := held.observed(r.adapter, snapshot, now)
	if err != nil {
		return Event{}, decisionWaiting, err
	}
	if !next.Supersedes(held) {
		return Event{}, decisionUnchanged, nil
	}
	return next, decisionAdvanced, nil
}

// JobKey formats the identity of a job the way Event.Key does, for a caller
// holding the parts rather than an event.
func JobKey(jobID int64, runAttempt int) string {
	return strconv.FormatInt(jobID, 10) + "/" + strconv.Itoa(runAttempt)
}
