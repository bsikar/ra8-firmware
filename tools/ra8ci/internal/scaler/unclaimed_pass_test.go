// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A revoker that records the reservations it was handed, so a pass can be
// judged by what it actually acted on rather than by its counters alone.
type recordingRevoker struct {
	revoked   []string
	destroyed []string
	released  []string
	abandoned []string
}

func (r *recordingRevoker) RevokeRegistration(_ context.Context, vm store.RunnerVM) error {
	r.revoked = append(r.revoked, vm.ID)
	return nil
}

func (r *recordingRevoker) DestroyGuest(_ context.Context, vm store.RunnerVM) error {
	r.destroyed = append(r.destroyed, vm.ID)
	return nil
}

func (r *recordingRevoker) ReleaseLease(_ context.Context, vm store.RunnerVM) error {
	r.released = append(r.released, vm.ID)
	return nil
}

func (r *recordingRevoker) AbandonAttempt(_ context.Context, vm store.RunnerVM) error {
	r.abandoned = append(r.abandoned, vm.ID)
	return nil
}

// The handler knows which scale set it serves, so the reaper it builds must
// be pointed at that one and no other.
func TestHandlerUnclaimedReaperTakesItsScaleSetFromTheHandler(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	ledger.vm = expiredReservation(handler.config.ScaleSetID + 1)
	revoker := &recordingRevoker{}
	report, err := handler.ReapUnclaimed(context.Background(), revoker)
	if err != nil {
		t.Fatalf("reap: %v", err)
	}
	if report.Scanned != 0 || len(revoker.abandoned) != 0 {
		t.Fatalf("reaped another scale set's reservation: %+v %v", report, revoker.abandoned)
	}
}

func TestHandlerReapUnclaimedWalksTheSequence(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	ledger.vm = expiredReservation(handler.config.ScaleSetID)
	revoker := &recordingRevoker{}
	report, err := handler.ReapUnclaimed(context.Background(), revoker)
	if err != nil {
		t.Fatalf("reap: %v", err)
	}
	if report.Scanned != 1 || report.Reaped != 1 || report.Claimed != 0 || report.Partial != 0 {
		t.Fatalf("report %+v", report)
	}
	for name, calls := range map[string][]string{
		StepRevokeRegistration: revoker.revoked,
		StepDestroyGuest:       revoker.destroyed,
		StepReleaseLease:       revoker.released,
		StepAbandonAttempt:     revoker.abandoned,
	} {
		if len(calls) != 1 || calls[0] != ledger.vm.ID {
			t.Fatalf("%s got %v, want the expired reservation once", name, calls)
		}
	}
	for _, step := range UnclaimedSteps() {
		if report.Steps[step] != 1 {
			t.Fatalf("step %s counted %d times", step, report.Steps[step])
		}
	}
}

// A reservation the job claimed is out of the reaper's reach for good, and
// the queue read is not the place that decides it.
func TestHandlerReapUnclaimedLeavesAClaimedReservationAlone(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	vm := expiredReservation(handler.config.ScaleSetID)
	claimed := vm.UnclaimedDeadline.Add(-time.Minute)
	vm.ClaimedAt = &claimed
	ledger.vm = vm
	revoker := &recordingRevoker{}
	report, err := handler.ReapUnclaimed(context.Background(), revoker)
	if err != nil {
		t.Fatalf("reap: %v", err)
	}
	if len(revoker.revoked) != 0 || len(revoker.abandoned) != 0 {
		t.Fatalf("acted on a claimed reservation: %v %v", revoker.revoked, revoker.abandoned)
	}
	_ = report
}

// The queue an operator inspects and the queue the reaper acts on are the
// same read, so a fresh reservation is absent from both.
func TestHandlerExpiredUnclaimedShowsTheSameQueueTheReaperTakes(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	vm := expiredReservation(handler.config.ScaleSetID)
	ledger.vm = vm
	expired, err := handler.ExpiredUnclaimed(context.Background(), vm.UnclaimedDeadline.Add(time.Second))
	if err != nil {
		t.Fatalf("queue: %v", err)
	}
	if len(expired) != 1 || expired[0].ID != vm.ID {
		t.Fatalf("queue held %d rows", len(expired))
	}
	fresh, err := handler.ExpiredUnclaimed(context.Background(), vm.UnclaimedDeadline.Add(-time.Minute))
	if err != nil {
		t.Fatalf("queue: %v", err)
	}
	if len(fresh) != 0 {
		t.Fatalf("queue held a reservation whose deadline has not passed: %+v", fresh)
	}
	if _, err := handler.ExpiredUnclaimed(context.Background(), time.Time{}); err == nil {
		t.Fatal("queue answered without a clock")
	}
}

// A deployment that already said how much work it wants in one pass has said
// it for this pass too.
func TestHandlerUnclaimedBatchFollowsTheReconcileBatch(t *testing.T) {
	handler, _, _, _, _ := testHarness(t)
	if got := handler.unclaimedReapBatch(); got != handler.config.MaxReconcileBatch {
		t.Fatalf("batch %d, want the handler's reconcile batch %d", got, handler.config.MaxReconcileBatch)
	}
	handler.config.MaxReconcileBatch = 0
	if got := handler.unclaimedReapBatch(); got != unclaimedReapBatch {
		t.Fatalf("unconfigured batch %d, want %d", got, unclaimedReapBatch)
	}
	handler.config.MaxReconcileBatch = 7
	if got := handler.unclaimedReapBatch(); got != 7 {
		t.Fatalf("configured batch %d, want 7", got)
	}
	handler.config.MaxReconcileBatch = 5000
	if got := handler.unclaimedReapBatch(); got != unclaimedReapBatch {
		t.Fatalf("out-of-range batch %d, want the default", got)
	}
}

func TestHandlerUnclaimedReaperRefusesAZeroHandler(t *testing.T) {
	var handler *Handler
	if _, err := handler.UnclaimedReaper(&recordingRevoker{}); err == nil {
		t.Fatal("built a reaper on a zero handler")
	}
	if _, err := handler.ExpiredUnclaimed(context.Background(), time.Now()); err == nil {
		t.Fatal("read the queue on a zero handler")
	}
	wired, _, _, _, _ := testHarness(t)
	if _, err := wired.UnclaimedReaper(nil); err == nil {
		t.Fatal("built a reaper with no revoker")
	}
}

func expiredReservation(scaleSetID int64) store.RunnerVM {
	deadline := time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)
	vm := store.RunnerVM{
		ID:                 "c1b2a3d4-0000-4000-8000-00000000000e",
		State:              "registered",
		Generation:         2,
		ExternalRunnerID:   5120,
		ExternalRunnerName: "ra8ci-1234567-1",
		UnclaimedDeadline:  deadline,
	}
	vm.ScaleSetID, vm.VMID = scaleSetID, 9000
	return vm
}
