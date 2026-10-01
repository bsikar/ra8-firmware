// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Events arrive over rows that have moved since the message was written. A
// started event meets the same unresolved mutation a completion can, and
// answers it the same way; a completion meets a reservation somebody already
// released, and has nothing left to do.

// A start over a row whose last mutation was never resolved, and whose
// operation is not fenced to that row, is refused. Registering identity on a
// reservation the plane cannot account for is how a live runner ends up
// attached to a guest nobody can name.
func TestAStartOverAnUnfencedUnknownOutcomeRecordsNoIdentity(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.UnknownOutcome = true
		row.CurrentOperationID = "0f0e0d0c-0000-4000-8000-00000000abcd"
		row.ExternalRunnerID = 0
		row.ExternalRunnerName = ""
	})

	if err := h.started(context.Background(), job); err == nil {
		t.Fatal("a start walked past an unresolved mutation")
	}

	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.ExternalRunnerID != 0 || after.ExternalRunnerName != "" {
		t.Fatalf("a refused start recorded runner identity anyway: %+v", after)
	}
}

// An unknown outcome with nothing recorded against it reconciles to a no-op,
// and the start carries on and records the runner the job names.
func TestAStartReconcilesAnEmptyUnknownOutcomeAndStillRegisters(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.UnknownOutcome = true
		row.CurrentOperationID = ""
		row.ExternalRunnerID = 0
		row.ExternalRunnerName = ""
	})

	if err := h.started(context.Background(), job); err != nil {
		t.Fatalf("a start refused a row with nothing to reconcile: %v", err)
	}

	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.ExternalRunnerID != int64(job.RunnerID) || after.ExternalRunnerName != job.RunnerName {
		t.Fatalf("the start recorded %d/%q, wanted the runner the job names",
			after.ExternalRunnerID, after.ExternalRunnerName)
	}
}

// A completion over a reservation somebody already released is accepted and
// changes nothing. The guest is gone; re-running the teardown would ask the
// hypervisor to delete what is not there and move a row that is finished.
func TestACompletionOverAnAlreadyReleasedReservationDoesNothing(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "released"
		row.CleanupRequested = true
	})
	before, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}

	if err := h.completed(context.Background(), completedJob(job)); err != nil {
		t.Fatalf("a completion over a released reservation was refused: %v", err)
	}

	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "released" || after.Generation != before.Generation {
		t.Fatalf("a finished reservation was moved: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a released reservation was torn down again: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// The two refusals a start can give over an unresolved row are not the same
// sentence as the one it gives for an identity mismatch, so an operator can
// tell an unaccountable reservation from a disagreeing message.
func TestAStartNamesTheUnresolvedOperationRatherThanTheIdentity(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.UnknownOutcome = true
		row.CurrentOperationID = "0f0e0d0c-0000-4000-8000-00000000abcd"
	})

	err := h.started(context.Background(), job)
	if err == nil || strings.Contains(err.Error(), "durable runner identity") {
		t.Fatalf("error = %v, want the unresolved operation named rather than the identity", err)
	}
}
