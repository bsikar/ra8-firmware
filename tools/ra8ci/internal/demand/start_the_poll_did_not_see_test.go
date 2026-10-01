// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package demand

import (
	"context"
	"testing"
	"time"
)

// polled is a snapshot at a phase with whatever stamps the test names.
func polled(phase Phase, startedAt, completedAt time.Time) JobSnapshot {
	snapshot := JobSnapshot{Phase: phase, StartedAt: startedAt, CompletedAt: completedAt}
	if phase == PhaseCompleted {
		snapshot.Conclusion = "success"
	}
	return snapshot
}

// A snapshot that read a start is the authority on it.
func TestASnapshotThatCarriesAStartWins(t *testing.T) {
	held := runningDemand(61, reconcileBase.Add(time.Minute), reconcileBase.Add(time.Minute))
	polledStart := reconcileBase.Add(2 * time.Minute)
	got := startTheJobAlreadyHad(held, polled(PhaseCompleted, polledStart, reconcileBase.Add(9*time.Minute)))
	if !got.Equal(polledStart) {
		t.Fatalf("start %s, want the snapshot's %s", got, polledStart)
	}
}

// Even an earlier one: a later reading of the forge's own stamp is a
// correction, not a loss.
func TestAnEarlierSnapshotStartIsStillTheSnapshotsToGive(t *testing.T) {
	held := runningDemand(62, reconcileBase.Add(5*time.Minute), reconcileBase.Add(5*time.Minute))
	earlier := reconcileBase.Add(time.Minute)
	if got := startTheJobAlreadyHad(held, polled(PhaseInProgress, earlier, time.Time{})); !got.Equal(earlier) {
		t.Fatalf("start %s, want the snapshot's %s", got, earlier)
	}
}

// The gap: a completed snapshot with no start keeps the start already held.
func TestACompletedSnapshotWithNoStartKeepsTheHeldStart(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(63, started, started)
	got := startTheJobAlreadyHad(held, polled(PhaseCompleted, time.Time{}, reconcileBase.Add(9*time.Minute)))
	if !got.Equal(started) {
		t.Fatalf("start %s, want the held %s: the poll did not read the field, it did not empty it", got, started)
	}
}

// Same for a snapshot still in progress.
func TestAnInProgressSnapshotWithNoStartKeepsTheHeldStart(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(64, started, started)
	if got := startTheJobAlreadyHad(held, polled(PhaseInProgress, time.Time{}, time.Time{})); !got.Equal(started) {
		t.Fatalf("start %s, want the held %s", got, started)
	}
}

// A snapshot that has regressed to queued carries no start into the event:
// a queued event holding a start is what checkStampsMatchThePhase refuses.
func TestAQueuedSnapshotCarriesNoStart(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(65, started, started)
	if got := startTheJobAlreadyHad(held, polled(PhaseQueued, time.Time{}, time.Time{})); !got.IsZero() {
		t.Fatalf("start %s, want none", got)
	}
}

// Nothing is invented: demand with no start on file and a snapshot with none
// still has none, and Validate still refuses the event that results.
func TestNoStartAnywhereStaysNoStart(t *testing.T) {
	held := queuedDemand(66, 1, reconcileBase)
	if got := startTheJobAlreadyHad(held, polled(PhaseCompleted, time.Time{}, reconcileBase.Add(time.Minute))); !got.IsZero() {
		t.Fatalf("start %s, want none", got)
	}
	if _, err := held.observed(ReconcileAdapter, polled(PhaseCompleted, time.Time{},
		reconcileBase.Add(time.Minute)), reconcileBase.Add(time.Hour)); err == nil {
		t.Fatal("a completion with no start anywhere must still be refused")
	}
}

// Through observed(): the merged event validates and keeps the held start.
func TestObservedKeepsAStartTheSnapshotDidNotCarry(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(67, started, started)
	completedAt := reconcileBase.Add(9 * time.Minute)
	next, err := held.observed(ReconcileAdapter, polled(PhaseCompleted, time.Time{}, completedAt),
		reconcileBase.Add(30*time.Minute))
	if err != nil {
		t.Fatalf("observed: %v", err)
	}
	if !next.StartedAt.Equal(started) {
		t.Errorf("start %s, want the held %s", next.StartedAt, started)
	}
	if next.Phase != PhaseCompleted || !next.CompletedAt.Equal(completedAt) {
		t.Errorf("snapshot not carried onto the event: %+v", next)
	}
	if err := next.Validate(); err != nil {
		t.Errorf("reconciled event does not validate: %v", err)
	}
}

// The runner name this rule is modelled on is untouched.
func TestObservedStillKeepsAHeldRunnerName(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(68, started, started)
	next, err := held.observed(ReconcileAdapter, polled(PhaseCompleted, time.Time{},
		reconcileBase.Add(9*time.Minute)), reconcileBase.Add(30*time.Minute))
	if err != nil {
		t.Fatalf("observed: %v", err)
	}
	if next.RunnerName != held.RunnerName {
		t.Errorf("runner name %q, want the held %q", next.RunnerName, held.RunnerName)
	}
}

// A snapshot that has regressed to queued is still judged not to supersede
// what is held, and the pass still leaves the row alone.
func TestAQueuedSnapshotIsStillUnchanged(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(69, started, started)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{69: polled(PhaseQueued, time.Time{}, time.Time{})}}
	store := newMemoryDemand(held)

	report, err := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute)).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Unchanged != 1 || report.Failed != 0 || report.Advanced != 0 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if len(store.recorded) != 0 {
		t.Fatalf("recorded %d events, want none", len(store.recorded))
	}
}

// End to end: the dropped completion this pass exists for, answered by a
// forge that no longer states the start, is settled instead of failed.
func TestPassSettlesACompletionWhoseSnapshotLostTheStart(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(70, started, started)
	completedAt := reconcileBase.Add(9 * time.Minute)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{70: polled(PhaseCompleted, time.Time{}, completedAt)}}
	store := newMemoryDemand(held)
	now := reconcileBase.Add(30 * time.Minute)

	report, err := reconcilerFor(t, source, store, now).Pass(context.Background())
	if err != nil {
		t.Fatalf("Pass: %v", err)
	}
	if report.Advanced != 1 || report.Failed != 0 {
		t.Fatalf("unexpected report: %+v", report)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("recorded %d events, want 1", len(store.recorded))
	}
	if got := store.recorded[0]; !got.StartedAt.Equal(started) || got.Phase != PhaseCompleted {
		t.Fatalf("recorded event lost the start: %+v", got)
	}
}

// And running the pass again over what it wrote records nothing new.
func TestTheSettledCompletionStaysSettled(t *testing.T) {
	started := reconcileBase.Add(time.Minute)
	held := runningDemand(71, started, started)
	source := &fakeJobs{jobs: map[int64]JobSnapshot{71: polled(PhaseCompleted, time.Time{},
		reconcileBase.Add(9*time.Minute))}}
	store := newMemoryDemand(held)
	reconciler := reconcilerFor(t, source, store, reconcileBase.Add(30*time.Minute))
	if _, err := reconciler.Pass(context.Background()); err != nil {
		t.Fatalf("first Pass: %v", err)
	}
	report, err := reconciler.Pass(context.Background())
	if err != nil {
		t.Fatalf("second Pass: %v", err)
	}
	if report.Scanned != 0 || report.Advanced != 0 || report.Failed != 0 {
		t.Fatalf("unexpected second report: %+v", report)
	}
	if len(store.recorded) != 1 {
		t.Fatalf("recorded %d events over two passes, want 1", len(store.recorded))
	}
}
