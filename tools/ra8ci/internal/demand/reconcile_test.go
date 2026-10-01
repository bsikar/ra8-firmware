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

var reconcileBase = time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

// fakeJobs is the mocked GitHub adapter: it answers about jobs it was told
// about and reports anything else as gone.
type fakeJobs struct {
	jobs  map[int64]JobSnapshot
	fail  map[int64]error
	calls []int64
}

func (f *fakeJobs) Job(ctx context.Context, owner, repository string, jobID int64) (JobSnapshot, bool, error) {
	f.calls = append(f.calls, jobID)
	if err := f.fail[jobID]; err != nil {
		return JobSnapshot{}, false, err
	}
	snapshot, ok := f.jobs[jobID]
	return snapshot, ok, nil
}

// memoryDemand holds demand the way the store does: keyed on the unit of
// demand, moved only by a strictly later phase.
type memoryDemand struct {
	held     map[string]Event
	order    []string
	recorded []Event
	failNext error
}

func newMemoryDemand(events ...Event) *memoryDemand {
	m := &memoryDemand{held: map[string]Event{}}
	for _, event := range events {
		m.held[event.Key()] = event
		m.order = append(m.order, event.Key())
	}
	return m
}

func (m *memoryDemand) ListOpen(ctx context.Context, limit int) ([]Event, error) {
	open := make([]Event, 0, len(m.order))
	for _, key := range m.order {
		event := m.held[key]
		if event.Phase != PhaseCompleted && len(open) < limit {
			open = append(open, event)
		}
	}
	return open, nil
}

func (m *memoryDemand) Record(ctx context.Context, event Event) error {
	if m.failNext != nil {
		err := m.failNext
		m.failNext = nil
		return err
	}
	m.recorded = append(m.recorded, event)
	if held, ok := m.held[event.Key()]; !ok || event.Supersedes(held) {
		m.held[event.Key()] = event
	}
	return nil
}

func queuedDemand(jobID int64, attempt int, observedAt time.Time) Event {
	return Event{Adapter: "github-app", DeliveryID: "d" + JobKey(jobID, attempt),
		Phase: PhaseQueued, JobID: jobID, RunID: 900, RunAttempt: attempt,
		Owner: "bsikar", Repository: "ra8-firmware", Workflow: "ci", JobName: "build",
		CommitSHA: strings.Repeat("a", 40), Labels: []string{"self-hosted", "ra8"},
		QueuedAt: reconcileBase, ObservedAt: observedAt}
}

func reconcilerFor(t *testing.T, source JobSource, store OpenDemandStore, now time.Time) *Reconciler {
	t.Helper()
	reconciler, err := NewReconciler(ReconcilerConfig{Source: source, Store: store,
		Grace: 2 * time.Minute, MissingAfter: time.Hour, BatchSize: 50,
		Now: func() time.Time { return now }})
	if err != nil {
		t.Fatalf("NewReconciler: %v", err)
	}
	return reconciler
}

// A completion delivery that never arrived is the case this pass exists for:
// the forge still knows the job finished, so the run becomes late, not silent.
func TestPassRecordsADroppedCompletion(t *testing.T) {
	held := queuedDemand(11, 1, reconcileBase)
	held.Phase = PhaseInProgress
	held.StartedAt = reconcileBase.Add(time.Minute)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{11: {Phase: PhaseCompleted,
		Conclusion: "success", RunnerName: "ra8-runner-7", StartedAt: reconcileBase.Add(time.Minute),
		CompletedAt: reconcileBase.Add(9 * time.Minute)}}}
	store := newMemoryDemand(held)
	now := reconcileBase.Add(30 * time.Minute)

	report, err := reconcilerFor(t, source, store, now).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Scanned != 1 || report.Advanced != 1 || report.Unchanged != 0 ||
		report.Waiting != 0 || report.Concluded != 0 || report.Failed != 0 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("recorded %d events, want 1", len(store.recorded))
	}
	got := store.recorded[0]
	if got.Key() != held.Key() {
		t.Errorf("reconciled event key %q, want %q: a poll must not mint a new unit of demand", got.Key(), held.Key())
	}
	if got.Adapter != ReconcileAdapter || got.DeliveryID != "reconcile.11.1.completed" {
		t.Errorf("adapter %q delivery %q", got.Adapter, got.DeliveryID)
	}
	if got.Phase != PhaseCompleted || got.Conclusion != "success" || got.RunnerName != "ra8-runner-7" {
		t.Errorf("snapshot not carried onto the event: %+v", got)
	}
	if got.Owner != held.Owner || got.CommitSHA != held.CommitSHA || !got.QueuedAt.Equal(held.QueuedAt) {
		t.Errorf("identity fields moved under the poll: %+v", got)
	}
	if !got.ObservedAt.Equal(now) {
		t.Errorf("observed at %s, want the pass clock %s", got.ObservedAt, now)
	}
	if err := got.Validate(); err != nil {
		t.Errorf("reconciled event does not validate: %v", err)
	}
}

// Running the pass again must not keep writing: the second pass sees the
// phase it already recorded and leaves the row alone.
func TestPassIsIdempotent(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{12: {Phase: PhaseCompleted, Conclusion: "failure",
		StartedAt: reconcileBase.Add(time.Minute), CompletedAt: reconcileBase.Add(5 * time.Minute)}}}
	store := newMemoryDemand(queuedDemand(12, 1, reconcileBase))
	reconciler := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute))

	first, err := reconciler.Pass(context.Background())
	if err != nil || first.Advanced != 1 {
		t.Fatalf("first pass: %+v err %v", first, err)
	}
	second, err := reconciler.Pass(context.Background())
	if err != nil {
		t.Fatalf("second pass: %v", err)
	}
	if second.Scanned != 0 || second.Advanced != 0 {
		t.Fatalf("second pass wrote again: %+v", second)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("recorded %d events across two passes, want 1", len(store.recorded))
	}
}

// The forge answering with an earlier phase than the one on file is an
// out-of-order read, and it must not walk the row backwards.
func TestPassNeverMovesDemandBackwards(t *testing.T) {
	held := queuedDemand(13, 1, reconcileBase)
	held.Phase = PhaseInProgress
	held.StartedAt = reconcileBase.Add(time.Minute)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{13: {Phase: PhaseQueued}}}
	store := newMemoryDemand(held)

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Unchanged != 1 || report.Advanced != 0 || len(store.recorded) != 0 {
		t.Fatalf("a backwards snapshot was written: %+v, recorded %d", report, len(store.recorded))
	}
}

// Inside the grace window a delivery is most likely still in flight, and
// polling there spends API budget racing the webhook.
func TestPassLeavesFreshDemandAlone(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{14: {Phase: PhaseCompleted, Conclusion: "success",
		StartedAt: reconcileBase, CompletedAt: reconcileBase.Add(time.Minute)}}}
	now := reconcileBase.Add(30 * time.Second)
	store := newMemoryDemand(queuedDemand(14, 1, now.Add(-time.Second)))

	report, err := reconcilerFor(t, source, store, now).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Waiting != 1 || report.Advanced != 0 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if len(source.calls) != 0 {
		t.Fatalf("polled the forge inside the grace window: %v", source.calls)
	}
}

// A job the forge no longer knows about waits out MissingAfter before it is
// concluded, and is then recorded as completed/stale so it stops being open.
func TestPassConcludesDemandTheForgeForgot(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{}}
	store := newMemoryDemand(queuedDemand(15, 2, reconcileBase))

	early, err := reconcilerFor(t, source, store, reconcileBase.Add(10*time.Minute)).Pass(context.Background())
	if err != nil {
		t.Fatalf("early pass: %v", err)
	}
	if early.Waiting != 1 || early.Concluded != 0 || len(store.recorded) != 0 {
		t.Fatalf("concluded a missing job too early: %+v", early)
	}

	now := reconcileBase.Add(2 * time.Hour)
	late, err := reconcilerFor(t, source, store, now).Pass(context.Background())
	if err != nil {
		t.Fatalf("late pass: %v", err)
	}
	if late.Concluded != 1 || late.Advanced != 0 || len(store.recorded) != 1 {
		t.Fatalf("unexpected late pass: %+v, recorded %d", late, len(store.recorded))
	}
	got := store.recorded[0]
	if got.Phase != PhaseCompleted || got.Conclusion != "stale" || got.Key() != "15/2" {
		t.Errorf("unexpected concluded event: %+v", got)
	}
	if got.StartedAt.IsZero() || got.CompletedAt.Before(got.QueuedAt) {
		t.Errorf("concluded event has incoherent times: %+v", got)
	}
	if err := got.Validate(); err != nil {
		t.Errorf("concluded event does not validate: %v", err)
	}
}

// One job the forge refuses to answer about must not cost the pass the rest
// of the batch, and the failure has to be reported rather than swallowed.
func TestPassKeepsGoingPastOneSourceFailure(t *testing.T) {
	source := &fakeJobs{
		jobs: map[int64]JobSnapshot{17: {Phase: PhaseCompleted, Conclusion: "success",
			StartedAt: reconcileBase, CompletedAt: reconcileBase.Add(time.Minute)}},
		fail: map[int64]error{16: errors.New("502 from the jobs API")}}
	store := newMemoryDemand(queuedDemand(16, 1, reconcileBase), queuedDemand(17, 1, reconcileBase))

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err == nil || !strings.Contains(err.Error(), "16/1") {
		t.Fatalf("pass hid the source failure: %v", err)
	}
	if report.Scanned != 2 || report.Failed != 1 || report.Advanced != 1 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if len(store.recorded) != 1 || store.recorded[0].JobID != 17 {
		t.Fatalf("the healthy job was not reconciled: %+v", store.recorded)
	}
}

// A forge failing on everything is a condition to report, not a batch to
// walk to the end.
func TestPassAbandonsAForgeThatKeepsFailing(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{}, fail: map[int64]error{}}
	events := make([]Event, 0, maxReconcileFailures+4)
	for i := range int64(maxReconcileFailures + 4) {
		source.fail[100+i] = errors.New("503")
		events = append(events, queuedDemand(100+i, 1, reconcileBase))
	}
	store := newMemoryDemand(events...)

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err == nil {
		t.Fatal("expected the pass to abandon a forge failing on every job")
	}
	if report.Failed != maxReconcileFailures {
		t.Fatalf("failed %d times, want the cap %d", report.Failed, maxReconcileFailures)
	}
	if len(source.calls) != maxReconcileFailures {
		t.Fatalf("kept calling after the cap: %d calls", len(source.calls))
	}
}

// A snapshot that does not describe a recordable state is that job's
// problem, not the pass's: nothing is written for it.
func TestPassRejectsAnIncoherentSnapshot(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{
		18: {Phase: PhaseInProgress},
		19: {Phase: PhaseCompleted, Conclusion: "not-a-conclusion",
			StartedAt: reconcileBase, CompletedAt: reconcileBase.Add(time.Minute)},
		20: {Phase: Phase("paused")}}}
	store := newMemoryDemand(queuedDemand(18, 1, reconcileBase),
		queuedDemand(19, 1, reconcileBase), queuedDemand(20, 1, reconcileBase))

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err == nil {
		t.Fatal("expected the incoherent snapshots to be reported")
	}
	if report.Failed != 3 || report.Advanced != 0 || len(store.recorded) != 0 {
		t.Fatalf("an incoherent snapshot was written: %+v, recorded %d", report, len(store.recorded))
	}
}

// A store that cannot take the write ends the pass: continuing would report
// progress the plane does not actually have.
func TestPassStopsOnAStoreFailure(t *testing.T) {
	source := &fakeJobs{jobs: map[int64]JobSnapshot{21: {Phase: PhaseCompleted, Conclusion: "success",
		StartedAt: reconcileBase, CompletedAt: reconcileBase.Add(time.Minute)}}}
	store := newMemoryDemand(queuedDemand(21, 1, reconcileBase))
	store.failNext = errors.New("connection refused")

	_, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err == nil || !strings.Contains(err.Error(), "record reconciled demand 21/1") {
		t.Fatalf("store failure not reported: %v", err)
	}
}

// Two attempts of the same job are two units of demand, and the pass must
// keep their evidence apart.
func TestPassKeepsRunAttemptsApart(t *testing.T) {
	first := queuedDemand(22, 1, reconcileBase)
	second := queuedDemand(22, 2, reconcileBase)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{22: {Phase: PhaseCompleted, Conclusion: "cancelled",
		StartedAt: reconcileBase, CompletedAt: reconcileBase.Add(time.Minute)}}}
	store := newMemoryDemand(first, second)

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Advanced != 2 || len(store.recorded) != 2 {
		t.Fatalf("unexpected report: %+v, recorded %d", report, len(store.recorded))
	}
	if a, b := store.recorded[0].DeliveryID, store.recorded[1].DeliveryID; a == b {
		t.Fatalf("both attempts recorded delivery %q", a)
	}
	if store.recorded[0].Key() == store.recorded[1].Key() {
		t.Fatalf("both attempts landed on key %q", store.recorded[0].Key())
	}
}

func TestReconcileDeliveryIDIsAValidDeliveryID(t *testing.T) {
	for _, phase := range []Phase{PhaseQueued, PhaseInProgress, PhaseCompleted} {
		id := reconcileDeliveryID(JobKey(4242, 7), phase)
		if !deliveryPattern.MatchString(id) {
			t.Errorf("delivery id %q does not match the adapter contract", id)
		}
	}
}

func TestNewReconcilerRejectsIncompleteConfiguration(t *testing.T) {
	good := ReconcilerConfig{Source: &fakeJobs{}, Store: newMemoryDemand(),
		Grace: time.Minute, MissingAfter: time.Hour, BatchSize: 10}
	if _, err := NewReconciler(good); err != nil {
		t.Fatalf("valid configuration rejected: %v", err)
	}
	cases := map[string]func(*ReconcilerConfig){
		"no source":        func(c *ReconcilerConfig) { c.Source = nil },
		"no store":         func(c *ReconcilerConfig) { c.Store = nil },
		"negative grace":   func(c *ReconcilerConfig) { c.Grace = -time.Second },
		"missing below gr": func(c *ReconcilerConfig) { c.MissingAfter = time.Second },
		"batch zero":       func(c *ReconcilerConfig) { c.BatchSize = 0 },
		"batch huge":       func(c *ReconcilerConfig) { c.BatchSize = 1001 },
	}
	for name, mutate := range cases {
		config := good
		mutate(&config)
		if _, err := NewReconciler(config); err == nil {
			t.Errorf("%s was accepted", name)
		}
	}
}
