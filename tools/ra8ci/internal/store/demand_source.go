// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// DemandSource is the store seen through the demand package's contracts: the
// reconciliation pass and the webhook endpoint both want somewhere to put a
// normalized event and, for the pass, the demand still open. The store's own
// methods deliberately do not match those interfaces, because they answer a
// richer question (DemandRecord, DemandOutcome) than either caller should be
// making decisions on. This type is the one place that translation happens.
type DemandSource struct {
	store *Store
}

// Compile-time proof that the adapter satisfies both directions of the
// contract, so a change to either interface fails the build here rather than
// at whichever binary wires them together.
var (
	_ demand.OpenDemandStore = (*DemandSource)(nil)
	_ demand.EventRecorder   = (*DemandSource)(nil)
)

// DemandSource returns the adapter. It is a view on the same store, holds no
// state of its own, and is safe to make per call site.
func (s *Store) DemandSource() *DemandSource { return &DemandSource{store: s} }

// ListOpen reads the demand that has not concluded, oldest queue time first.
// The bookkeeping the store keeps around an event (first seen, version) is
// dropped here on purpose: the pass decides from the event and the clock, and
// a version it cannot fence on is a field it could only misuse.
func (d *DemandSource) ListOpen(ctx context.Context, limit int) ([]demand.Event, error) {
	if d == nil || d.store == nil {
		return nil, fmt.Errorf("%w: demand source", ErrInvalid)
	}
	records, err := d.store.ListOpenDemand(ctx, limit)
	if err != nil {
		return nil, err
	}
	return demandEvents(records), nil
}

// Record stores one event and reports only whether the plane now holds it.
// Every DemandOutcome means it does: accepted and superseded wrote, stale and
// duplicate mean something at least as new was already on file. Collapsing
// them here is what makes at-least-once delivery and a repeated poll safe for
// callers that have no business branching on which of the four happened.
func (d *DemandSource) Record(ctx context.Context, event demand.Event) error {
	if d == nil || d.store == nil {
		return fmt.Errorf("%w: demand source", ErrInvalid)
	}
	outcome, err := d.store.RecordDemandEvent(ctx, event)
	return recordedDemand(outcome, err)
}

// demandEvents projects stored rows onto the events the demand package
// speaks.
func demandEvents(records []DemandRecord) []demand.Event {
	events := make([]demand.Event, 0, len(records))
	for _, record := range records {
		events = append(events, record.Event)
	}
	return events
}

// recordedDemand is the outcome-to-error rule with no I/O in it. A write that
// reported an outcome at all settled the event; an error is a real failure,
// including ErrConflict, which means a delivery id is describing two
// different units of demand and one of them is wrong.
func recordedDemand(outcome DemandOutcome, err error) error {
	if err != nil {
		return fmt.Errorf("record demand event: %w", err)
	}
	switch outcome {
	case DemandAccepted, DemandSuperseded, DemandStale, DemandDuplicate:
		return nil
	default:
		return fmt.Errorf("%w: unrecognized demand outcome %q", ErrUnavailable, outcome)
	}
}

// DemandOutcomes is every outcome a write can report, so a test (and the
// switch above) cannot silently fall behind a new one.
func DemandOutcomes() []DemandOutcome {
	return []DemandOutcome{DemandAccepted, DemandSuperseded, DemandStale, DemandDuplicate}
}
