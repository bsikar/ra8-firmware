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

// The unclaimed destroy is the one teardown with no job completion behind it,
// so every refusal on the way to it carries weight. These hold the re-read
// that opens the step and the evidence that has to be assembled before a
// guest is deleted.

// rereadLedger answers the destroyer's opening re-read from what the test
// holds, leaving the rest of the ledger to the harness.
type rereadLedger struct {
	*memoryLedger
	fresh   store.RunnerVM
	failure error
	reads   int
}

func (r *rereadLedger) GetRunnerVM(_ context.Context, _ string) (store.RunnerVM, error) {
	r.reads++
	if r.failure != nil {
		return store.RunnerVM{}, r.failure
	}
	return r.fresh, nil
}

func TestClaimedAtNamesTheTimeOrSaysItIsUnknown(t *testing.T) {
	if got := claimedAt(store.RunnerVM{}); got != "an unknown time" {
		t.Fatalf("an unclaimed row = %q", got)
	}
	at := time.Date(2026, 9, 28, 5, 46, 0, 0, time.FixedZone("CDT", -5*60*60))
	vm := store.RunnerVM{ClaimedAt: &at}
	if got := claimedAt(vm); got != "2026-09-28T10:46:00Z" {
		t.Fatalf("a claimed row = %q, want the instant in UTC", got)
	}
}

func TestDestroyingAGuestRefusesAPartialCall(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	var absent *UnclaimedDestroyer
	if err := absent.DestroyUnclaimedGuest(context.Background(), store.RunnerVM{}); err == nil ||
		!strings.Contains(err.Error(), "invalid unclaimed destroyer") {
		t.Fatalf("nil destroyer = %v", err)
	}
	if err := (&UnclaimedDestroyer{}).DestroyUnclaimedGuest(context.Background(), store.RunnerVM{}); err == nil {
		t.Fatal("a destroyer with no wiring destroyed a guest")
	}
	if err := destroyer.DestroyUnclaimedGuest(nil, store.RunnerVM{}); err == nil ||
		!strings.Contains(err.Error(), "invalid unclaimed destroyer") {
		t.Fatalf("nil context = %v", err)
	}
	if forge.calls != 0 {
		t.Fatalf("a refused call still asked the forge %d time(s)", forge.calls)
	}
}

// The batch the reaper built is stale by the time the step runs, so the row
// is re-read first. A read that fails leaves the reservation queued rather
// than tearing anything down on the older copy.
func TestAReservationThatCannotBeRereadIsLeftQueued(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	unreadable := errors.New("reservation row is unreadable")
	h.ledger = &rereadLedger{memoryLedger: ledger, failure: unreadable}
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
	if !errors.Is(err, unreadable) || !strings.Contains(err.Error(), vm.ID) {
		t.Fatalf("destroy = %v, want the read failure naming the reservation", err)
	}
	if forge.calls != 0 {
		t.Fatalf("the forge was asked %d time(s) before the row could be read", forge.calls)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("hypervisor touched on an unreadable row: stop=%d delete=%d", fake.stopCalls, fake.deleteCalls)
	}
}

// A ledger answering with someone else's reservation is a conflict, not a
// row to act on: the destroy would otherwise be pointed at another guest.
func TestALedgerAnsweringWithAnotherReservationIsRefused(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	other := vm
	other.ID = "01996f90-3415-7cfe-8ff1-600058131c00"
	h.ledger = &rereadLedger{memoryLedger: ledger, fresh: other}
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("destroy = %v, want a conflict", err)
	}
	if !strings.Contains(err.Error(), other.ID) || !strings.Contains(err.Error(), vm.ID) {
		t.Fatalf("destroy = %v, want both reservations named", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("hypervisor touched on a mismatched row: stop=%d delete=%d", fake.stopCalls, fake.deleteCalls)
	}
}

// The operator's cleanup approval is not implied by the reservation being
// unclaimed: without a reviewed approval there is no destroy evidence to
// assemble at all.
func TestDestroyEvidenceNeedsAReviewedCleanupApproval(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	for name, approval := range map[string]string{
		"no approval at all":    "",
		"not a reviewed ID":     "cleanup-please",
		"an approval-shaped id": "01996f90-3415-7cfe-8ff1",
	} {
		h.config.CleanupApprovalID = approval
		_, err := destroyer.destroyProof(context.Background(), vm)
		if err == nil || !strings.Contains(err.Error(), "needs a reviewed cleanup approval") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// The guest is observed inside the destroy attempt, so the digest and the
// freshness window belong to this attempt. A guest that is still up is not
// safe to delete, whatever the ledger says about the reservation.
func TestDestroyEvidenceRefusesAGuestThatIsNotSafeToDelete(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	_, err = destroyer.destroyProof(context.Background(), vm)
	if err == nil || !strings.Contains(err.Error(), "is not safe for cleanup") {
		t.Fatalf("a running guest = %v", err)
	}
}

// And a reservation written under approvals that have since changed never
// reaches the hypervisor at all: the identity fence refuses it first.
func TestDestroyEvidenceRefusesAReservationOutsideApprovals(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	vm.Node = "someone-elses-node"
	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	before := func() int {
		fake.mu.Lock()
		defer fake.mu.Unlock()
		return fake.stopCalls + fake.deleteCalls
	}()
	_, err = destroyer.destroyProof(context.Background(), vm)
	if err == nil || !strings.Contains(err.Error(), "durable VM identity differs from current approvals") {
		t.Fatalf("a reservation outside approvals = %v", err)
	}
	if after := func() int {
		fake.mu.Lock()
		defer fake.mu.Unlock()
		return fake.stopCalls + fake.deleteCalls
	}(); after != before {
		t.Fatalf("the hypervisor was mutated on a refused identity: %d -> %d", before, after)
	}
}
