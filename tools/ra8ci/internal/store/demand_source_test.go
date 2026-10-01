// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

func sourceDemandFixture(jobID int64, attempt int, phase demand.Phase) demand.Event {
	queued := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	event := demand.Event{Adapter: demand.WebhookAdapter, DeliveryID: "d-1", Phase: phase,
		JobID: jobID, RunID: 55, RunAttempt: attempt, Owner: "bsikar", Repository: "ra8-firmware",
		Workflow: "ci", JobName: "build", CommitSHA: "0123456789abcdef0123456789abcdef01234567",
		Labels: []string{"self-hosted"}, QueuedAt: queued, ObservedAt: queued.Add(time.Second)}
	if phase != demand.PhaseQueued {
		event.StartedAt = queued.Add(time.Minute)
	}
	if phase == demand.PhaseCompleted {
		event.Conclusion = "success"
		event.CompletedAt = queued.Add(2 * time.Minute)
	}
	return event
}

// Every outcome the store can report means the plane now holds the event, so
// the adapter must swallow all four. A new outcome that nobody taught the
// adapter about has to fail loudly instead of reading as success.
func TestRecordedDemandSwallowsEveryOutcome(t *testing.T) {
	for _, outcome := range DemandOutcomes() {
		if err := recordedDemand(outcome, nil); err != nil {
			t.Fatalf("outcome %q should settle the event, got %v", outcome, err)
		}
	}
	if err := recordedDemand(DemandOutcome("invented"), nil); err == nil {
		t.Fatal("an unrecognized outcome must not read as a settled event")
	} else if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("unrecognized outcome should be ErrUnavailable, got %v", err)
	}
}

// A conflict is a delivery id describing two different units of demand. It
// is the one write result that must reach the caller, and it must stay
// matchable with errors.Is through the adapter's wrapping.
func TestRecordedDemandPreservesWriteFailures(t *testing.T) {
	for _, sentinel := range []error{ErrConflict, ErrInvalid, ErrUnavailable} {
		err := recordedDemand("", sentinel)
		if err == nil {
			t.Fatalf("write failure %v must not be swallowed", sentinel)
		}
		if !errors.Is(err, sentinel) {
			t.Fatalf("wrapped error lost its sentinel: %v", err)
		}
	}
}

// The pass reads events, not bookkeeping: the projection must carry the
// event through untouched and in the order the store listed it.
func TestDemandEventsProjectsRecordsInOrder(t *testing.T) {
	records := []DemandRecord{
		{Event: sourceDemandFixture(11, 1, demand.PhaseQueued), Version: 1},
		{Event: sourceDemandFixture(12, 2, demand.PhaseInProgress), Version: 7},
	}
	events := demandEvents(records)
	if len(events) != len(records) {
		t.Fatalf("projected %d events from %d records", len(events), len(records))
	}
	for i, event := range events {
		if event.Key() != records[i].Event.Key() {
			t.Fatalf("event %d is %s, want %s", i, event.Key(), records[i].Event.Key())
		}
		if event.Phase != records[i].Event.Phase || event.ObservedAt != records[i].Event.ObservedAt {
			t.Fatalf("event %d lost a field the pass decides on: %+v", i, event)
		}
	}
	if got := demandEvents(nil); got == nil || len(got) != 0 {
		t.Fatalf("no open demand should project to an empty slice, got %#v", got)
	}
}

// The projection must not lose what the reconciler needs to build its next
// event: it merges a snapshot onto what ListOpen handed it, so a field
// dropped here becomes an invalid write later.
func TestProjectedEventStillValidates(t *testing.T) {
	record := DemandRecord{Event: sourceDemandFixture(21, 1, demand.PhaseInProgress),
		FirstSeenAt: time.Now(), UpdatedAt: time.Now(), Version: 3}
	events := demandEvents([]DemandRecord{record})
	if err := events[0].Validate(); err != nil {
		t.Fatalf("projected event no longer satisfies the adapter contract: %v", err)
	}
}

// A zero adapter is a programming error, not a panic at the first delivery.
func TestDemandSourceRefusesWithoutAStore(t *testing.T) {
	var source *DemandSource
	if _, err := source.ListOpen(context.Background(), 10); !errors.Is(err, ErrInvalid) {
		t.Fatalf("nil source should refuse to list, got %v", err)
	}
	if err := (&DemandSource{}).Record(context.Background(), sourceDemandFixture(31, 1, demand.PhaseQueued)); !errors.Is(err, ErrInvalid) {
		t.Fatalf("storeless source should refuse to record, got %v", err)
	}
}
