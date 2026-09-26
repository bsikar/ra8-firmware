// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The scale set every test here reaps. reaperAt in unclaimed_reaper_test.go
// builds its reaper over the same number, so a row built by reservationIn is
// interchangeable with one built there.
const reapedScaleSet = 42

func reservationIn(id string, set int64, deadline time.Time) store.RunnerVM {
	vm := store.RunnerVM{ID: id, State: "registered", UnclaimedDeadline: deadline}
	vm.ScaleSetID = set
	return vm
}

// scaleSetQueue answers the list with whatever it was given and the re-read
// from its own map, so a test can make the two reads disagree about the scale
// set without touching the candidate rows.
type scaleSetQueue struct {
	rows    []store.RunnerVM
	current map[string]store.RunnerVM
	reads   int
}

func (q *scaleSetQueue) ListExpiredUnclaimedRunnerVMs(context.Context, int64, time.Time, int) ([]store.RunnerVM, error) {
	return q.rows, nil
}

func (q *scaleSetQueue) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	q.reads++
	if vm, ok := q.current[id]; ok {
		return vm, nil
	}
	for _, row := range q.rows {
		if row.ID == id {
			return row, nil
		}
	}
	return store.RunnerVM{}, store.ErrNotFound
}

// touchedRevoker records which reservations reached a system, so a test can
// assert that a refused row cost nothing at the forge or the hypervisor.
type touchedRevoker struct {
	touched []string
}

func (r *touchedRevoker) mark(step string, vm store.RunnerVM) error {
	r.touched = append(r.touched, step+":"+vm.ID)
	return nil
}

func (r *touchedRevoker) RevokeRegistration(_ context.Context, vm store.RunnerVM) error {
	return r.mark(StepRevokeRegistration, vm)
}
func (r *touchedRevoker) DestroyGuest(_ context.Context, vm store.RunnerVM) error {
	return r.mark(StepDestroyGuest, vm)
}
func (r *touchedRevoker) ReleaseLease(_ context.Context, vm store.RunnerVM) error {
	return r.mark(StepReleaseLease, vm)
}
func (r *touchedRevoker) AbandonAttempt(_ context.Context, vm store.RunnerVM) error {
	return r.mark(StepAbandonAttempt, vm)
}

func TestAReservationInThisScaleSetPasses(t *testing.T) {
	vm := reservationIn("res-1", reapedScaleSet, time.Now())
	if err := checkReservationScaleSet(reapedScaleSet, unclaimedQueueRead, vm); err != nil {
		t.Fatalf("a row from the reaped scale set: %v", err)
	}
	if err := checkReservationScaleSet(reapedScaleSet, unclaimedRereadRead, vm); err != nil {
		t.Fatalf("the same row from the re-read: %v", err)
	}
}

func TestAReservationFromAnotherScaleSetIsAConflict(t *testing.T) {
	for _, set := range []int64{0, 1, 41, 43, 9000} {
		vm := reservationIn("res-1", set, time.Now())
		err := checkReservationScaleSet(reapedScaleSet, unclaimedQueueRead, vm)
		if !errors.Is(err, store.ErrConflict) {
			t.Fatalf("scale set %d: want ErrConflict, got %v", set, err)
		}
	}
}

func TestTheRefusalNamesBothScaleSetsAndTheRead(t *testing.T) {
	vm := reservationIn("res-7", 7, time.Now())
	err := checkReservationScaleSet(reapedScaleSet, unclaimedRereadRead, vm)
	if err == nil {
		t.Fatal("want a refusal")
	}
	for _, want := range []string{"res-7", "7", "42", unclaimedRereadRead} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %q", err, want)
		}
	}
}

func TestAPassWithNoScaleSetCannotJudgeARow(t *testing.T) {
	vm := reservationIn("res-1", reapedScaleSet, time.Now())
	for _, asked := range []int64{0, -1} {
		err := checkReservationScaleSet(asked, unclaimedQueueRead, vm)
		if !errors.Is(err, store.ErrInvalid) {
			t.Fatalf("asked %d: want ErrInvalid, got %v", asked, err)
		}
	}
	if err := checkReservationScaleSet(reapedScaleSet, "", vm); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("unnamed read: want ErrInvalid, got %v", err)
	}
}

func TestTheQueueAnsweringAboutAnotherScaleSetEndsThePass(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	queue := &scaleSetQueue{rows: []store.RunnerVM{
		reservationIn("res-1", reapedScaleSet, now.Add(-time.Hour)),
		reservationIn("res-2", 43, now.Add(-time.Hour)),
		reservationIn("res-3", reapedScaleSet, now.Add(-time.Hour)),
	}}
	revoker := &touchedRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, ErrUnclaimedIncomplete) || !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want an incomplete pass over a conflict, got %v", err)
	}
	if report.Scanned != 1 {
		t.Fatalf("scanned %d, want only the row judged before the foreign one", report.Scanned)
	}
	if report.Reaped != 1 {
		t.Fatalf("reaped %d, want the first row reaped", report.Reaped)
	}
	for _, touched := range revoker.touched {
		if strings.HasSuffix(touched, ":res-2") || strings.HasSuffix(touched, ":res-3") {
			t.Fatalf("a row after the foreign one reached a system: %q", touched)
		}
	}
}

func TestAForeignRowIsRefusedBeforeAnySystemIsTouched(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	queue := &scaleSetQueue{rows: []store.RunnerVM{reservationIn("res-9", 43, now.Add(-time.Hour))}}
	revoker := &touchedRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want a conflict, got %v", err)
	}
	if len(revoker.touched) != 0 {
		t.Fatalf("a foreign row reached %v", revoker.touched)
	}
	if queue.reads != 0 {
		t.Fatalf("a foreign row was re-read %d times, want 0", queue.reads)
	}
	if report.Scanned != 0 || report.Reaped != 0 || report.Partial != 0 {
		t.Fatalf("a refused pass counted %+v", report)
	}
}

func TestTheRereadMovingScaleSetsEndsThePass(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	queue := &scaleSetQueue{
		rows: []store.RunnerVM{reservationIn("res-4", reapedScaleSet, now.Add(-time.Hour))},
		current: map[string]store.RunnerVM{
			"res-4": reservationIn("res-4", 43, now.Add(-time.Hour)),
		},
	}
	revoker := &touchedRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if !errors.Is(err, ErrUnclaimedIncomplete) || !errors.Is(err, store.ErrConflict) {
		t.Fatalf("want an incomplete pass over a conflict, got %v", err)
	}
	if len(revoker.touched) != 0 {
		t.Fatalf("a row whose re-read moved scale sets reached %v", revoker.touched)
	}
	if report.Scanned != 1 || report.Reaped != 0 {
		t.Fatalf("report %+v, want the row scanned and not reaped", report)
	}
}

func TestAnOrdinaryPassIsUnchangedByTheDoor(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	queue := &scaleSetQueue{rows: []store.RunnerVM{
		reservationIn("res-1", reapedScaleSet, now.Add(-time.Hour)),
		reservationIn("res-2", reapedScaleSet, now.Add(-time.Minute)),
	}}
	revoker := &touchedRevoker{}
	report, err := reaperAt(t, now, queue, revoker).Reap(context.Background())
	if err != nil {
		t.Fatalf("an ordinary pass: %v", err)
	}
	if report.Scanned != 2 || report.Reaped != 2 || report.Partial != 0 {
		t.Fatalf("report %+v, want two scanned and two reaped", report)
	}
	for _, step := range UnclaimedSteps() {
		if report.Steps[step] != 2 {
			t.Fatalf("step %s counted %d, want 2", step, report.Steps[step])
		}
	}
	if len(revoker.touched) != 8 {
		t.Fatalf("touched %v, want four steps for each of two reservations", revoker.touched)
	}
}

func TestTheTwoReadsAreNamedDifferently(t *testing.T) {
	if unclaimedQueueRead == unclaimedRereadRead {
		t.Fatal("the two reads must be distinguishable in a refusal")
	}
	if unclaimedQueueRead == "" || unclaimedRereadRead == "" {
		t.Fatal("a read with no name cannot be reported")
	}
}
