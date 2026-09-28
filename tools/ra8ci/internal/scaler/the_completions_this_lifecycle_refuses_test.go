// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A completion whose row names a guest this handler was never configured to
// touch is refused on identity, before the ledger is moved or the hypervisor
// is asked anything.
func TestACompletionRefusesAGuestOutsideThisHandlersConfiguredSet(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) { row.VMID = 4242 })
	ctx := context.Background()
	if err := h.completed(ctx, completedJob(job)); err == nil {
		t.Fatal("a completion accepted a guest outside the configured set")
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.CleanupRequested {
		t.Fatalf("a refused completion requested cleanup: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a refused completion touched the hypervisor: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// A job that finished against a reservation no guest was ever cloned for is
// not something this step abandons on its own. Closing the row would say a
// guest was cleaned up when none existed to clean, so it is reported and an
// operator attests to the absence instead.
func TestACompletionWillNotAbandonANeverCreatedReservationOnItsOwn(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) { row.State = "reserved" })
	err := h.completed(context.Background(), completedJob(job))
	if err == nil || !strings.Contains(err.Error(), "operator absence attestation") {
		t.Fatalf("error = %v, want a refusal asking for an absence attestation", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a never-created reservation was torn down: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// Cleanup already requested, but the guest is mid-clone. There is no rule for
// destroying from here, so the state is quoted back rather than forced, and
// the row waits for the reconciler that does know what a half-cloned guest
// is.
func TestACompletionQuotesAStateItHasNoDestroyRuleFor(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "cloning"
		row.CleanupRequested = true
	})
	err := h.completed(context.Background(), completedJob(job))
	if err == nil || !strings.Contains(err.Error(), `"cloning"`) {
		t.Fatalf("error = %v, want the unexpected state quoted", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.deleteCalls != 0 {
		t.Fatalf("a half-cloned guest was destroyed: delete=%d", fake.deleteCalls)
	}
}

// A started event for a reservation already being cleaned up is accepted and
// does nothing. The job is on its way out, and registering a runner against a
// draining row would put identity on a reservation about to be released.
func TestAStartedEventOnAReservationBeingCleanedUpIsANoOp(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) { row.CleanupRequested = true })
	ctx := context.Background()
	if err := h.started(ctx, job); err != nil {
		t.Fatalf("a started event during cleanup was refused: %v", err)
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "running" || after.ExternalRunnerID != vm.ExternalRunnerID {
		t.Fatalf("a no-op started event moved the row: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.startCalls != 1 {
		t.Fatalf("a no-op started event asked the hypervisor: start=%d", fake.startCalls)
	}
}

// A started event whose row names a guest outside the configured set is
// refused on identity, the same way a completion is, and never reaches the
// runner API.
func TestAStartedEventRefusesAGuestOutsideThisHandlersConfiguredSet(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) { row.VMID = 4242 })
	observer := &answeringObserver{}
	h.runners = observer
	if err := h.started(context.Background(), job); err == nil {
		t.Fatal("a started event accepted a guest outside the configured set")
	}
	if observer.drainCalls != 0 {
		t.Fatalf("the runner API was asked during a refusal: %d", observer.drainCalls)
	}
}
