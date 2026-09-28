// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Before a start is sent the plane looks the guest up. Two things can go
// wrong there and they are not the same thing: the hypervisor may not answer
// for the guest at all, or it may answer with a guest that is not in a state
// worth starting. The first is a question nobody answered; the second is an
// answer the plane refuses.

// stoppedReservation puts a reservation in the state prepareAndStart is
// entered from, and hands back the row as the ledger holds it.
func stoppedReservation(t *testing.T, ledger *memoryLedger) store.RunnerVM {
	t.Helper()
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.State = "stopped"
		row.CleanupRequested = false
		row.UnknownOutcome = false
	})
	row, err := ledger.GetRunnerVM(context.Background(), ledger.vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	return row
}

// startsSent reads how many start requests the hypervisor has been sent so
// far, so a refusal can be judged on what it added rather than on a total the
// reservation's own setup already moved.
func startsSent(fake *fakeProxmox) int {
	fake.mu.Lock()
	defer fake.mu.Unlock()
	return fake.startCalls
}

// A guest the hypervisor does not list is carried back as the lookup failure
// itself, not as an opinion about the guest's state.
func TestAStartWaitsWhenTheHypervisorDoesNotKnowTheGuest(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	row := stoppedReservation(t, ledger)
	fake.mu.Lock()
	fake.exists = false
	fake.mu.Unlock()
	before := startsSent(fake)

	err := h.prepareAndStart(context.Background(), row)
	if !errors.Is(err, proxmox.ErrNotFound) {
		t.Fatalf("error = %v, want the lookup failure carried back", err)
	}
	if strings.Contains(err.Error(), "not stable and stopped") {
		t.Fatalf("an unanswered lookup was reported as an unstable guest: %v", err)
	}

	if sent := startsSent(fake) - before; sent != 0 {
		t.Fatalf("a start was sent for a guest nobody could find: %d", sent)
	}
}

// A guest that is listed but running is a different answer, and reads as one.
// Starting it again would be a second mutation on a guest already doing work.
func TestAStartRefusesAGuestThatIsNotStopped(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	row := stoppedReservation(t, ledger)
	fake.mu.Lock()
	fake.exists = true
	fake.status = "running"
	fake.mu.Unlock()
	before := startsSent(fake)

	err := h.prepareAndStart(context.Background(), row)
	if err == nil || !strings.Contains(err.Error(), "not stable and stopped") {
		t.Fatalf("error = %v, want the unstable guest named", err)
	}
	if errors.Is(err, proxmox.ErrNotFound) {
		t.Fatalf("a listed guest was reported as missing: %v", err)
	}

	if sent := startsSent(fake) - before; sent != 0 {
		t.Fatalf("a running guest was started again: %d", sent)
	}
}

// The reservation is untouched by either refusal. Neither answer is evidence
// about the guest, so neither may move the row.
func TestNeitherPreStartRefusalMovesTheReservation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	row := stoppedReservation(t, ledger)

	for _, missing := range []bool{true, false} {
		fake.mu.Lock()
		fake.exists = !missing
		fake.status = "running"
		fake.mu.Unlock()

		if err := h.prepareAndStart(context.Background(), row); err == nil {
			t.Fatalf("missing=%v: a start went ahead", missing)
		}
		after, err := ledger.GetRunnerVM(context.Background(), row.ID)
		if err != nil {
			t.Fatal(err)
		}
		if after.State != "stopped" || after.Generation != row.Generation || after.CleanupRequested {
			t.Fatalf("missing=%v: the reservation moved: %+v", missing, after)
		}
	}
}
