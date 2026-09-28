// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"math"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// rowAs edits the single reservation the memory ledger holds. The destroyer
// re-reads the row itself, so a test states the situation the reaper would
// have found rather than driving the guest there through the forge.
func rowAs(t *testing.T, ledger *memoryLedger, edit func(vm *store.RunnerVM)) {
	t.Helper()
	ledger.mu.Lock()
	defer ledger.mu.Unlock()
	edit(&ledger.vm)
}

// destroyerRefusal runs the destroyer over the row as it now stands and
// insists it refused without touching the hypervisor. Every case below shares
// this assertion: the point of a refusal here is that no guest was stopped or
// deleted on the strength of a reservation the plane could not stand behind.
func destroyerRefusal(t *testing.T, h *Handler, ledger *memoryLedger, fake *fakeProxmox, vm store.RunnerVM) error {
	t.Helper()
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	stops, deletes := fake.stopCalls, fake.deleteCalls
	fake.mu.Unlock()

	err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
	if err == nil {
		t.Fatal("destroyer accepted a reservation it cannot stand behind")
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != stops || fake.deleteCalls != deletes {
		t.Fatalf("hypervisor asked during a refusal: stop %d->%d delete %d->%d",
			stops, fake.stopCalls, deletes, fake.deleteCalls)
	}
	return err
}

// A reservation whose cleanup was already requested but whose guest is still
// running is not something this step can finish. The sequence stops with the
// state named, because an operator reading the pass report needs to know the
// row was reached and left alone rather than skipped silently.
func TestADestroyRefusesAReservationThatIsNotReadyAndNamesItsState(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "running"
		row.CleanupRequested = true
	})
	err := destroyerRefusal(t, h, ledger, fake, vm)
	if !errors.Is(err, ErrUnclaimedIncomplete) {
		t.Fatalf("error = %v, want an incomplete teardown", err)
	}
	if !strings.Contains(err.Error(), `"running"`) {
		t.Fatalf("refusal does not name the state it found: %v", err)
	}
}

// The same holds one step further along: cleanup requested, the guest already
// gone as far as the ledger is concerned, but the row left in a state with no
// destroy rule. It is reported, not forced.
func TestADestroyRefusesAClonedReservationWithCleanupAlreadyRequested(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "cloning"
		row.CleanupRequested = true
	})
	if err := destroyerRefusal(t, h, ledger, fake, vm); !errors.Is(err, ErrUnclaimedIncomplete) {
		t.Fatalf("error = %v, want an incomplete teardown", err)
	}
}

// A runner id the forge could never have issued is refused before the forge
// is asked anything. The ledger row and the registration would be talking
// about different runners, and every later step in the sequence acts on that
// disagreement.
func TestADestroyRefusesARunnerIdentifierOutsideTheForgesRange(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.ExternalRunnerID = math.MaxInt32 + 1
	})
	if err := destroyerRefusal(t, h, ledger, fake, vm); !strings.Contains(err.Error(), "outside the forge's range") {
		t.Fatalf("error = %v, want a refusal naming the forge's range", err)
	}
}

// A reservation ready to destroy, but naming a guest this handler was never
// configured to touch. The identity is refused before the hypervisor is asked
// to observe anything, which is what keeps one scale set from deleting
// another's guest on the strength of a stale row.
func TestADestroyRefusesAGuestOutsideThisHandlersConfiguredSet(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "stopped"
		row.CleanupRequested = true
		row.VMID = 4242
	})
	if err := destroyerRefusal(t, h, ledger, fake, vm); err == nil {
		t.Fatal("a foreign guest was accepted for cleanup")
	}
}

// Ready by the ledger's account, still running by the hypervisor's. The
// digest and the state are read off the guest at destroy time for exactly
// this reason, so the disagreement stops the teardown.
func TestADestroyRefusesAGuestTheHypervisorStillReportsRunning(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "stopped"
		row.CleanupRequested = true
	})
	fake.mu.Lock()
	fake.status = "running"
	fake.mu.Unlock()
	if err := destroyerRefusal(t, h, ledger, fake, vm); !strings.Contains(err.Error(), "not safe for cleanup") {
		t.Fatalf("error = %v, want a refusal on the observed guest", err)
	}
}

// And the positive control for the pair above: the same row, with the guest
// actually stopped, is destroyed. Without this the refusals could all be
// passing for some reason other than the one under test.
func TestADestroyProceedsOnceTheGuestAgreesItIsStopped(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "stopped"
		row.CleanupRequested = true
	})
	fake.mu.Lock()
	fake.status = "stopped"
	fake.mu.Unlock()
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.deleteCalls != 1 || fake.stopCalls != 0 || fake.exists {
		t.Fatalf("stopped guest not destroyed in one step: stop=%d delete=%d exists=%v",
			fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}
