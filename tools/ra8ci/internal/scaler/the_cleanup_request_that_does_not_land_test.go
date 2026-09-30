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

// The cleanup request is the one write the unclaimed sequence makes before it
// touches the hypervisor, and what it answers decides whether anything is
// torn down at all. These hold the two ways that write can end badly.

// drainingLedger answers the cleanup request from what the test holds,
// leaving the rest of the ledger to the harness.
type drainingLedger struct {
	*memoryLedger
	marked  store.RunnerVM
	failure error
	marks   int
}

func (d *drainingLedger) MarkRunnerVMDraining(_ context.Context, _, _ string, _ int64) (store.RunnerVM, error) {
	d.marks++
	if d.failure != nil {
		return store.RunnerVM{}, d.failure
	}
	return d.marked, nil
}

// A cleanup request that does not land leaves the guest alone. The refusal
// names the reservation, because an operator reading it has no other way to
// tell which row the ledger refused.
func TestAReservationWhoseCleanupCannotBeRequestedIsLeftAlone(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	refused := errors.New("cleanup column is not writable")
	marking := &drainingLedger{memoryLedger: ledger, failure: refused}
	h.ledger = marking

	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
	if !errors.Is(err, refused) {
		t.Fatalf("destroy = %v, want the ledger's refusal", err)
	}
	if !strings.Contains(err.Error(), vm.ID) {
		t.Fatalf("the refusal does not name the reservation: %v", err)
	}
	if marking.marks != 1 {
		t.Fatalf("cleanup was requested %d time(s), want exactly one attempt", marking.marks)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("hypervisor touched without a cleanup request: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// A reservation claimed as the cleanup request lands is refused on the row
// the ledger just answered with, not on the copy the sequence started from.
// The guest is never stopped: the claim arrived first, so the job that took
// it is still entitled to the machine.
func TestAReservationClaimedAsCleanupIsRequestedIsNeverStopped(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	claimed := vm
	claimed.State = "draining"
	claimed.CleanupRequested = true
	claimed.ClaimedAt = claimedNow()
	h.ledger = &drainingLedger{memoryLedger: ledger, marked: claimed}

	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("destroy = %v, want a conflict on the freshly claimed row", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a claimed reservation reached the hypervisor: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// And the same evidence is refused for want of a clock, which is the other
// way unclaimedSafety declines to speak for a reservation.
func TestUnclaimedSafetyNeedsBothAnUnclaimedRowAndAClock(t *testing.T) {
	vm := store.RunnerVM{ID: "a"}
	if _, err := unclaimedSafety(vm, time.Time{}); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("evidence without a clock: %v", err)
	}
	proof, err := unclaimedSafety(vm, time.Now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	if proof.EvidenceID == "" || proof.ObservedAt.IsZero() {
		t.Fatalf("evidence is not identified or dated: %+v", proof)
	}
}
