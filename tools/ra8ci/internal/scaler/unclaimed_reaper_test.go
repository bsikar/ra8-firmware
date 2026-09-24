// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"reflect"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type fakeUnclaimedQueue struct {
	rows  []store.RunnerVM
	err   error
	calls int
	now   time.Time
	limit int
	set   int64
}

func (q *fakeUnclaimedQueue) ListExpiredUnclaimedRunnerVMs(_ context.Context, set int64, now time.Time, limit int) ([]store.RunnerVM, error) {
	q.calls++
	q.set, q.now, q.limit = set, now, limit
	return q.rows, q.err
}

type fakeRevoker struct {
	order []string
	fail  map[string]error
}

func (r *fakeRevoker) run(step string, vm store.RunnerVM) error {
	r.order = append(r.order, step+":"+vm.ID)
	return r.fail[step]
}

func (r *fakeRevoker) RevokeRegistration(_ context.Context, vm store.RunnerVM) error {
	return r.run(StepRevokeRegistration, vm)
}
func (r *fakeRevoker) DestroyGuest(_ context.Context, vm store.RunnerVM) error {
	return r.run(StepDestroyGuest, vm)
}
func (r *fakeRevoker) ReleaseLease(_ context.Context, vm store.RunnerVM) error {
	return r.run(StepReleaseLease, vm)
}
func (r *fakeRevoker) AbandonAttempt(_ context.Context, vm store.RunnerVM) error {
	return r.run(StepAbandonAttempt, vm)
}

func expiredVM(id string, deadline time.Time) store.RunnerVM {
	return store.RunnerVM{ID: id, State: "registered", UnclaimedDeadline: deadline}
}

func reaperAt(t *testing.T, now time.Time, queue ExpiredUnclaimedLister, revoker UnclaimedRevoker) *UnclaimedReaper {
	t.Helper()
	reaper, err := NewUnclaimedReaper(UnclaimedReaperConfig{
		ScaleSetID: 42, BatchSize: 10, Now: func() time.Time { return now },
	}, queue, revoker)
	if err != nil {
		t.Fatal(err)
	}
	return reaper
}

func TestUnclaimedReaperRunsTheStepsInOrder(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{
		expiredVM("a", now.Add(-time.Hour)),
		expiredVM("b", now.Add(-time.Minute)),
	}}
	revoker := &fakeRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		StepRevokeRegistration + ":a", StepDestroyGuest + ":a",
		StepReleaseLease + ":a", StepAbandonAttempt + ":a",
		StepRevokeRegistration + ":b", StepDestroyGuest + ":b",
		StepReleaseLease + ":b", StepAbandonAttempt + ":b",
	}
	if !reflect.DeepEqual(revoker.order, want) {
		t.Fatalf("step order %v, want %v", revoker.order, want)
	}
	if report.Scanned != 2 || report.Reaped != 2 || report.Partial != 0 || report.Claimed != 0 {
		t.Fatalf("report %+v", report)
	}
	for _, step := range UnclaimedSteps() {
		if report.Steps[step] != 2 {
			t.Fatalf("step %s counted %d times, want 2", step, report.Steps[step])
		}
	}
	if queue.set != 42 || queue.limit != 10 || !queue.now.Equal(now) {
		t.Fatalf("queue read with set=%d limit=%d now=%s", queue.set, queue.limit, queue.now)
	}
}

// Revocation comes first for a reason: while the registration is live the
// runner can accept a job, so a failed revocation must stop everything after
// it rather than destroy a guest that may be about to take work.
func TestUnclaimedReaperStopsAtTheFailedStep(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	boom := errors.New("forge refused")
	for _, stop := range UnclaimedSteps() {
		queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{expiredVM("a", now.Add(-time.Hour))}}
		revoker := &fakeRevoker{fail: map[string]error{stop: boom}}
		report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
		if !errors.Is(err, ErrUnclaimedIncomplete) || !errors.Is(err, boom) {
			t.Fatalf("stop at %s: %v", stop, err)
		}
		if report.Reaped != 0 || report.Partial != 1 || report.Scanned != 1 {
			t.Fatalf("stop at %s: report %+v", stop, report)
		}
		var ran []string
		for _, step := range UnclaimedSteps() {
			ran = append(ran, step)
			if step == stop {
				break
			}
		}
		if len(revoker.order) != len(ran) {
			t.Fatalf("stop at %s ran %v, want to stop after %v", stop, revoker.order, ran)
		}
		if report.Steps[stop] != 0 {
			t.Fatalf("stop at %s counted the step it failed", stop)
		}
	}
}

// A job can take the runner between the queue read and the reservation's turn
// in the batch. Revoking then would kill live work, so the row is skipped.
func TestUnclaimedReaperLeavesAReservationClaimedUnderIt(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	claimedAt := now.Add(-time.Minute)
	claimed := expiredVM("claimed", now.Add(-time.Hour))
	claimed.ClaimedAt = &claimedAt
	released := expiredVM("released", now.Add(-time.Hour))
	released.State = "released"
	queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{claimed, released, expiredVM("open", now.Add(-time.Hour))}}
	revoker := &fakeRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if report.Scanned != 3 || report.Claimed != 2 || report.Reaped != 1 {
		t.Fatalf("report %+v", report)
	}
	for _, entry := range revoker.order {
		if entry[len(entry)-len("open"):] != "open" {
			t.Fatalf("reaper touched a reservation it should have skipped: %v", revoker.order)
		}
	}
}

// One reservation failing costs that reservation. A collaborator refusing
// everything ends the pass instead of being asked once per row.
func TestUnclaimedReaperAbandonsAPassThatFailsThroughout(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	var rows []store.RunnerVM
	for i := 0; i < maxUnclaimedFailures+5; i++ {
		rows = append(rows, expiredVM(string(rune('a'+i)), now.Add(-time.Hour)))
	}
	queue := &fakeUnclaimedQueue{rows: rows}
	revoker := &fakeRevoker{fail: map[string]error{StepRevokeRegistration: errors.New("forge down")}}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, ErrUnclaimedIncomplete) {
		t.Fatalf("pass error %v", err)
	}
	if report.Scanned != maxUnclaimedFailures || report.Partial != maxUnclaimedFailures {
		t.Fatalf("pass kept going: %+v", report)
	}
}

func TestUnclaimedReaperQueueFailureIsNotAPartialPass(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	boom := errors.New("ledger unavailable")
	queue := &fakeUnclaimedQueue{err: boom}
	revoker := &fakeRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, boom) {
		t.Fatalf("queue failure %v", err)
	}
	if report.Scanned != 0 || len(revoker.order) != 0 {
		t.Fatalf("queue failure still revoked something: %+v %v", report, revoker.order)
	}
}

func TestNewUnclaimedReaperRefusesWhatItCannotRun(t *testing.T) {
	queue, revoker := &fakeUnclaimedQueue{}, &fakeRevoker{}
	if _, err := NewUnclaimedReaper(UnclaimedReaperConfig{ScaleSetID: 0}, queue, revoker); err == nil {
		t.Fatal("no scale set accepted")
	}
	if _, err := NewUnclaimedReaper(UnclaimedReaperConfig{ScaleSetID: 1}, nil, revoker); err == nil {
		t.Fatal("no queue accepted")
	}
	if _, err := NewUnclaimedReaper(UnclaimedReaperConfig{ScaleSetID: 1}, queue, nil); err == nil {
		t.Fatal("no revoker accepted")
	}
	if _, err := NewUnclaimedReaper(UnclaimedReaperConfig{ScaleSetID: 1, BatchSize: 5000}, queue, revoker); err == nil {
		t.Fatal("unbounded batch accepted")
	}
	reaper, err := NewUnclaimedReaper(UnclaimedReaperConfig{ScaleSetID: 1}, queue, revoker)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := reaper.Reap(context.Background()); err != nil {
		t.Fatal(err)
	}
	if queue.limit != 50 || queue.now.IsZero() {
		t.Fatalf("defaults not applied: limit=%d now=%s", queue.limit, queue.now)
	}
}

// The sequence is the contract. If a step is added to the order without a
// call behind it, every reservation must fail loudly rather than quietly skip
// the new step.
func TestUnclaimedStepsAreAllImplemented(t *testing.T) {
	steps := UnclaimedSteps()
	if len(steps) != 4 || steps[0] != StepRevokeRegistration {
		t.Fatalf("sequence %v does not start by revoking the registration", steps)
	}
	seen := map[string]bool{}
	for _, step := range steps {
		if seen[step] {
			t.Fatalf("step %s listed twice", step)
		}
		seen[step] = true
	}
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{expiredVM("a", now.Add(-time.Hour))}}
	revoker := &fakeRevoker{}
	if _, err := reaperAt(t, now, queue, revoker).Reap(context.Background()); err != nil {
		t.Fatal(err)
	}
	if len(revoker.order) != len(steps) {
		t.Fatalf("%d steps ran for %d in the sequence", len(revoker.order), len(steps))
	}
}
