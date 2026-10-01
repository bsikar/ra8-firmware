// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// A pass that did not fill its batch saw all the open demand there was, and
// says so by leaving the mark off.
func TestAPassThatDidNotFillItsBatchIsNotTruncated(t *testing.T) {
	for _, shape := range []struct {
		held, batch int
	}{{0, 1}, {0, 50}, {1, 2}, {9, 10}, {49, 50}} {
		truncated, err := checkOpenDemandFitsTheBatch(make([]Event, shape.held), shape.batch)
		if err != nil || truncated {
			t.Fatalf("%d of %d: truncated=%v err=%v", shape.held, shape.batch, truncated, err)
		}
	}
}

// A full batch is the one shape that says nothing about the demand behind it,
// which is what the mark exists to report.
func TestAFullBatchIsReportedAsTruncated(t *testing.T) {
	for _, size := range []int{1, 2, 10, 50, 1000} {
		truncated, err := checkOpenDemandFitsTheBatch(make([]Event, size), size)
		if err != nil || !truncated {
			t.Fatalf("batch of %d: truncated=%v err=%v", size, truncated, err)
		}
	}
}

// More than was asked for is a store the pass can believe nothing else from:
// the limit is the only thing bounding the requests this pass makes to the
// forge.
func TestMoreDemandThanTheBatchAskedForIsRefused(t *testing.T) {
	truncated, err := checkOpenDemandFitsTheBatch(make([]Event, 11), 10)
	if !errors.Is(err, errOpenDemandOverflows) || truncated {
		t.Fatalf("an oversized list was accepted: truncated=%v err=%v", truncated, err)
	}
	if !strings.Contains(err.Error(), "11") || !strings.Contains(err.Error(), "10") {
		t.Fatalf("refusal named neither number: %v", err)
	}
}

// One unit either side of the boundary, so the mark is not passing by luck of
// a coarse fixture.
func TestTheBoundaryIsExact(t *testing.T) {
	for _, shape := range []struct {
		held, batch int
		truncated   bool
		refused     bool
	}{{9, 10, false, false}, {10, 10, true, false}, {11, 10, false, true}} {
		truncated, err := checkOpenDemandFitsTheBatch(make([]Event, shape.held), shape.batch)
		if (err != nil) != shape.refused || truncated != shape.truncated {
			t.Fatalf("%d of %d: truncated=%v err=%v", shape.held, shape.batch, truncated, err)
		}
	}
}

// The pass carries the mark, and carries it only when the batch is full. The
// counters are unchanged either way, because the mark says what they cover
// and not what they are.
func TestTheMarkReachesTheReport(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	for _, shape := range []struct {
		open, batch int
		want        bool
	}{{2, 5, false}, {3, 3, true}} {
		store := &batchDemandStore{}
		for i := 0; i < shape.open; i++ {
			store.open = append(store.open, openUnitAt(int64(100+i), now.Add(-time.Hour)))
		}
		reconciler, err := NewReconciler(ReconcilerConfig{
			Source: unchangedJobSource{startedAt: now.Add(-time.Hour)}, Store: store, Grace: time.Minute,
			MissingAfter: time.Hour, BatchSize: shape.batch,
			Now: func() time.Time { return now },
		})
		if err != nil {
			t.Fatal(err)
		}
		report, err := reconciler.Pass(context.Background())
		if err != nil {
			t.Fatal(err)
		}
		if report.Truncated != shape.want || report.Scanned != shape.open {
			t.Fatalf("%d of %d: %+v", shape.open, shape.batch, report)
		}
	}
}

// A store answering with more than the limit ends the pass before a single
// request is made to the forge, because the requests are the cost being
// bounded.
func TestAnOversizedListEndsThePassBeforeTheForgeIsAsked(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	store := &batchDemandStore{}
	for i := 0; i < 4; i++ {
		store.open = append(store.open, openUnitAt(int64(200+i), now.Add(-time.Hour)))
	}
	source := &countingJobSource{startedAt: now.Add(-time.Hour)}
	reconciler, err := NewReconciler(ReconcilerConfig{
		Source: source, Store: store, Grace: time.Minute, MissingAfter: time.Hour,
		BatchSize: 2, Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatal(err)
	}
	report, err := reconciler.Pass(context.Background())
	if !errors.Is(err, errOpenDemandOverflows) {
		t.Fatalf("oversized list walked: %+v %v", report, err)
	}
	if source.calls != 0 || report.Scanned != 0 {
		t.Fatalf("the forge was asked %d times past the refusal: %+v", source.calls, report)
	}
}

// The starvation this mark reports: with more open demand than one batch
// holds, and a head the forge agrees is unchanged, the same oldest units come
// back every pass and the demand behind them is never asked about. The mark
// is the only thing in the report that says so.
func TestAStableFullBatchKeepsReportingTruncated(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	store := &batchDemandStore{}
	for i := 0; i < 2; i++ {
		store.open = append(store.open, openUnitAt(int64(300+i), now.Add(-time.Hour)))
	}
	source := &countingJobSource{startedAt: now.Add(-time.Hour)}
	reconciler, err := NewReconciler(ReconcilerConfig{
		Source: source, Store: store, Grace: time.Minute, MissingAfter: time.Hour,
		BatchSize: 2, Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatal(err)
	}
	for pass := 0; pass < 3; pass++ {
		report, err := reconciler.Pass(context.Background())
		if err != nil || !report.Truncated || report.Scanned != 2 {
			t.Fatalf("pass %d: %+v %v", pass, report, err)
		}
	}
	if len(store.recorded) != 0 {
		t.Fatalf("an unchanged head was rewritten: %+v", store.recorded)
	}
}

// openUnitAt is one unit of open demand, queued and observed at the same
// moment, which is the state ListOpen hands the pass.
func openUnitAt(jobID int64, at time.Time) Event {
	return Event{
		Adapter: "github-app", DeliveryID: "d" + JobKey(jobID, 1), Owner: "bsikar",
		Repository: "ra8-firmware", Workflow: "ci", JobName: "build",
		CommitSHA: strings.Repeat("a", 40), Labels: []string{"self-hosted", "ra8"},
		JobID: jobID, RunID: 900, RunAttempt: 1,
		Phase: PhaseInProgress, QueuedAt: at, StartedAt: at, ObservedAt: at,
	}
}

type batchDemandStore struct {
	open     []Event
	recorded []Event
}

func (s *batchDemandStore) ListOpen(context.Context, int) ([]Event, error) {
	return append([]Event(nil), s.open...), nil
}

func (s *batchDemandStore) Record(_ context.Context, event Event) error {
	s.recorded = append(s.recorded, event)
	return nil
}

// unchangedJobSource answers with the phase and start the demand already
// holds, so the pass decides unchanged and writes nothing.
type unchangedJobSource struct{ startedAt time.Time }

func (s unchangedJobSource) Job(_ context.Context, _, _ string, _ int64) (JobSnapshot, bool, error) {
	return JobSnapshot{Phase: PhaseInProgress, StartedAt: s.startedAt}, true, nil
}

type countingJobSource struct {
	startedAt time.Time
	calls     int
}

func (s *countingJobSource) Job(_ context.Context, _, _ string, _ int64) (JobSnapshot, bool, error) {
	s.calls++
	return JobSnapshot{Phase: PhaseInProgress, StartedAt: s.startedAt}, true, nil
}
