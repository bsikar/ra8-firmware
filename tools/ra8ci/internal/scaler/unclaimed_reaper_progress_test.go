// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// What a failing pass reports is all an operator has to work from, because
// the rows stay in the queue either way and the question is always which
// system is refusing. These pin the counts to the steps that actually ran.

func oneExpired(now time.Time) []store.RunnerVM {
	return []store.RunnerVM{expiredVM("a", now.Add(-time.Hour))}
}

func TestAPartialReservationCountsTheStepsItFinished(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	boom := errors.New("bench refused")
	queue := &fakeUnclaimedQueue{rows: oneExpired(now)}
	revoker := &fakeRevoker{fail: map[string]error{StepReleaseLease: boom}}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, ErrUnclaimedIncomplete) || !errors.Is(err, boom) {
		t.Fatalf("pass error %v", err)
	}
	if report.Reaped != 0 || report.Partial != 1 {
		t.Fatalf("report %+v", report)
	}
	if report.Steps[StepRevokeRegistration] != 1 || report.Steps[StepDestroyGuest] != 1 {
		t.Fatalf("finished steps not counted: %v", report.Steps)
	}
	if report.Steps[StepReleaseLease] != 0 || report.Steps[StepAbandonAttempt] != 0 {
		t.Fatalf("counted a step that did not finish: %v", report.Steps)
	}
}

// The whole point of counting per step: two passes failing on every row must
// not report the same thing when they are failing in different places.
func TestAPassStoppingEarlyAndOneStoppingLateReportDifferently(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	boom := errors.New("collaborator down")
	early := &fakeRevoker{fail: map[string]error{StepRevokeRegistration: boom}}
	late := &fakeRevoker{fail: map[string]error{StepAbandonAttempt: boom}}

	earlyReport, err := reaperAt(t, now, &fakeUnclaimedQueue{rows: oneExpired(now)}, early).Reap(context.Background())
	if !errors.Is(err, boom) {
		t.Fatalf("early pass %v", err)
	}
	lateReport, err := reaperAt(t, now, &fakeUnclaimedQueue{rows: oneExpired(now)}, late).Reap(context.Background())
	if !errors.Is(err, boom) {
		t.Fatalf("late pass %v", err)
	}
	if earlyReport.Partial != lateReport.Partial {
		t.Fatalf("fixtures disagree: %+v %+v", earlyReport, lateReport)
	}
	if len(earlyReport.Steps) != 0 {
		t.Fatalf("a pass that never got past the forge counted %v", earlyReport.Steps)
	}
	if len(lateReport.Steps) != 3 {
		t.Fatalf("a pass that only failed to close the row counted %v", lateReport.Steps)
	}
}

// A reservation the reaper skips has had no work done on it.
func TestAClaimedReservationCountsNoSteps(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	claimedAt := now.Add(-time.Minute)
	claimed := expiredVM("claimed", now.Add(-time.Hour))
	claimed.ClaimedAt = &claimedAt
	queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{claimed}}
	report, err := reaperAt(t, now, queue, &fakeRevoker{}).Reap(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if report.Claimed != 1 || len(report.Steps) != 0 {
		t.Fatalf("report %+v", report)
	}
}

// Counts accumulate across a batch: two reservations stopping at the same
// place report that place twice, which is how a pass says where it is stuck.
func TestStepCountsAccumulateAcrossTheBatch(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	queue := &fakeUnclaimedQueue{rows: []store.RunnerVM{
		expiredVM("a", now.Add(-time.Hour)),
		expiredVM("b", now.Add(-time.Hour)),
	}}
	revoker := &fakeRevoker{fail: map[string]error{StepDestroyGuest: errors.New("hypervisor down")}}
	report, _ := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if report.Steps[StepRevokeRegistration] != 2 {
		t.Fatalf("revocations counted %d times, want 2: %v", report.Steps[StepRevokeRegistration], report.Steps)
	}
	if report.Steps[StepDestroyGuest] != 0 {
		t.Fatalf("counted the failing step: %v", report.Steps)
	}
}

// A reservation that walks the whole sequence still counts each step exactly
// once, so moving the counting into the walk cannot inflate a clean pass.
func TestACompletedReservationStillCountsEveryStepOnce(t *testing.T) {
	now := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	queue := &fakeUnclaimedQueue{rows: oneExpired(now)}
	report, err := reaperAt(t, now, queue, &fakeRevoker{}).Reap(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if report.Reaped != 1 || len(report.Steps) != len(UnclaimedSteps()) {
		t.Fatalf("report %+v", report)
	}
	for _, step := range UnclaimedSteps() {
		if report.Steps[step] != 1 {
			t.Fatalf("step %s counted %d times, want 1", step, report.Steps[step])
		}
	}
}
