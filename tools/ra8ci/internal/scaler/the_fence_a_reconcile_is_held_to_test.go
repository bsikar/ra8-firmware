// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A reservation carrying an unknown outcome names the operation it is waiting
// on. Before anything is reconciled against the hypervisor, that operation has
// to be fenced to this reservation and this generation: an operation the
// ledger happens to hold under the named ID is not the same thing as the
// operation this row is actually waiting on, and reconciling against the wrong
// one would resolve a reservation on another's evidence.

// unresolvedAgainst plants an operation in the ledger and points the
// reservation at it, leaving the caller to say how the two disagree.
func unresolvedAgainst(t *testing.T, ledger *memoryLedger, shape func(vm store.RunnerVM, op *store.RunnerVMOperation)) store.RunnerVM {
	t.Helper()
	row, err := ledger.GetRunnerVM(context.Background(), ledger.vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	op := store.RunnerVMOperation{
		ID: row.CreationOperationID, RunnerVMID: row.ID, Kind: "clone",
		FromState: "reserved", PendingState: "cloning", Generation: row.Generation, Status: "unresolved",
	}
	shape(row, &op)
	rowAs(t, ledger, func(r *store.RunnerVM) {
		r.UnknownOutcome = true
		r.CurrentOperationID = op.ID
	})
	ledger.mu.Lock()
	ledger.op = op
	ledger.mu.Unlock()
	row, err = ledger.GetRunnerVM(context.Background(), row.ID)
	if err != nil {
		t.Fatal(err)
	}
	return row
}

// Three ways an operation can fail to be this reservation's, each refused
// before a single request reaches the hypervisor.
func TestAnOperationThatIsNotThisReservationsIsRefusedBeforeReconciling(t *testing.T) {
	for _, attempt := range []struct {
		name  string
		shape func(vm store.RunnerVM, op *store.RunnerVMOperation)
	}{
		{"it belongs to another reservation", func(vm store.RunnerVM, op *store.RunnerVMOperation) {
			op.RunnerVMID = vm.ID + "-other"
		}},
		{"it was opened against an older generation", func(vm store.RunnerVM, op *store.RunnerVMOperation) {
			op.Generation = vm.Generation - 1
		}},
		{"it was already resolved", func(vm store.RunnerVM, op *store.RunnerVMOperation) {
			op.Status = "resolved"
		}},
	} {
		h, ledger, fake, _, job := testHarness(t)
		expired(t, h, ledger, job)
		row := unresolvedAgainst(t, ledger, attempt.shape)
		before := startsSent(fake)

		_, err := h.reconcileOne(context.Background(), row)
		if err == nil {
			t.Errorf("%s: an unfenced operation was reconciled", attempt.name)
			continue
		}
		if !strings.Contains(err.Error(), "not fenced to reservation") {
			t.Errorf("%s: the refusal read %v", attempt.name, err)
		}
		if sent := startsSent(fake) - before; sent != 0 {
			t.Errorf("%s: %d requests were sent on an unfenced operation", attempt.name, sent)
		}
	}
}

// The refusal is about the fence, not about unknown outcomes in general: an
// operation that IS this reservation's gets past it and fails, if at all, on
// its own evidence.
func TestAFencedOperationIsNotRefusedForItsFence(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	row := unresolvedAgainst(t, ledger, func(store.RunnerVM, *store.RunnerVMOperation) {})

	if _, err := h.reconcileOne(context.Background(), row); err != nil &&
		strings.Contains(err.Error(), "not fenced to reservation") {
		t.Fatalf("a fenced operation was refused as unfenced: %v", err)
	}
}

// A row with nothing outstanding is handed straight back, untouched, without
// the ledger being asked for an operation at all.
func TestAReservationWithNothingOutstandingIsHandedBack(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	row, err := ledger.GetRunnerVM(context.Background(), ledger.vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	rowAs(t, ledger, func(r *store.RunnerVM) {
		r.UnknownOutcome = false
		r.CurrentOperationID = ""
	})
	row.UnknownOutcome = false
	row.CurrentOperationID = ""

	back, err := h.reconcileOne(context.Background(), row)
	if err != nil {
		t.Fatalf("a settled reservation was refused: %v", err)
	}
	if back.ID != row.ID || back.Generation != row.Generation || back.State != row.State {
		t.Fatalf("a settled reservation came back changed: %+v", back)
	}
}
