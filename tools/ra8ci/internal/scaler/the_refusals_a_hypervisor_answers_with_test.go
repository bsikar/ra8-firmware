// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"
)

// A hypervisor that refuses the stop is not a reason to carry on to the
// destroy. The intent is already committed in the ledger at that point, so
// the step hands the failure back and leaves the row for the reconciler
// rather than deleting a guest whose stop nobody can account for.
func TestACompletionThatCannotStopTheGuestNeverDestroysIt(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := registeredReservation(t, h, ledger, job)
	h.runners = &answeringObserver{drained: soundDrain(t)}
	fake.mu.Lock()
	fake.stopRefused = true
	fake.mu.Unlock()

	ctx := context.Background()
	if err := h.completed(ctx, completedJob(job)); err == nil {
		t.Fatal("a refused stop was reported as a finished completion")
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State == "released" {
		t.Fatalf("a reservation was closed over a refused stop: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 1 || fake.deleteCalls != 0 || !fake.exists {
		t.Fatalf("stop=%d delete=%d exists=%v, want one refused stop and no destroy",
			fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}

// The same one step later: the guest is down, the destroy is refused. The
// reservation stays open, because a row marked released while its guest is
// still on the hypervisor is how a guest is leaked.
func TestACompletionThatCannotDestroyTheGuestLeavesTheReservationOpen(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := registeredReservation(t, h, ledger, job)
	h.runners = &answeringObserver{drained: soundDrain(t)}
	fake.mu.Lock()
	fake.deleteRefused = true
	fake.mu.Unlock()

	ctx := context.Background()
	if err := h.completed(ctx, completedJob(job)); err == nil {
		t.Fatal("a refused destroy was reported as a finished completion")
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State == "released" {
		t.Fatalf("a reservation was closed over a refused destroy: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 1 || fake.deleteCalls != 1 || !fake.exists {
		t.Fatalf("stop=%d delete=%d exists=%v, want the stop done and the destroy refused",
			fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}

// The unclaimed reaper meets the same two failures, and names the guest in
// both so an operator reading the pass report knows which VMID to look at.
func TestAnUnclaimedTeardownNamesTheGuestWhenTheHypervisorRefuses(t *testing.T) {
	for _, tc := range []struct {
		name   string
		arm    func(f *fakeProxmox)
		expect string
	}{
		{"stop refused", func(f *fakeProxmox) { f.stopRefused = true }, "stop unclaimed guest 9000"},
		{"destroy refused", func(f *fakeProxmox) { f.deleteRefused = true }, "destroy unclaimed guest 9000"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			h, ledger, fake, _, job := testHarness(t)
			vm := expired(t, h, ledger, job)
			fake.mu.Lock()
			tc.arm(fake)
			fake.mu.Unlock()

			destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
			if err != nil {
				t.Fatal(err)
			}
			err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
			if err == nil || !strings.Contains(err.Error(), tc.expect) {
				t.Fatalf("error = %v, want one naming %q", err, tc.expect)
			}
			after, readErr := ledger.GetRunnerVM(context.Background(), vm.ID)
			if readErr != nil {
				t.Fatal(readErr)
			}
			if after.State == "released" {
				t.Fatalf("an unclaimed reservation was closed over a refusal: %+v", after)
			}
			fake.mu.Lock()
			defer fake.mu.Unlock()
			if !fake.exists {
				t.Fatal("the guest was deleted despite the refusal")
			}
		})
	}
}

// And the control the three above lean on: with the hypervisor answering
// normally, the very same completion closes the reservation and removes the
// guest. Without it the refusals could be passing on some unrelated failure.
func TestACompletionOverAWillingHypervisorClosesTheReservation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := registeredReservation(t, h, ledger, job)
	h.runners = &answeringObserver{drained: soundDrain(t)}
	ctx := context.Background()
	if err := h.completed(ctx, completedJob(job)); err != nil {
		t.Fatal(err)
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "released" || !after.CleanupRequested {
		t.Fatalf("completion did not close the reservation: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 1 || fake.deleteCalls != 1 || fake.exists {
		t.Fatalf("stop=%d delete=%d exists=%v, want one of each and the guest gone",
			fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}
