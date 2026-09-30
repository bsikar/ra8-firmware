// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The two refusals destroyProof makes AFTER the approval and the identity
// fence: one about the guest it could not observe, one about a reservation
// that stopped being unclaimed while the attempt was under way.

// A guest the hypervisor cannot describe is not evidence of an empty node.
// Naming the guest in the refusal is what lets an operator tell "the API did
// not answer" apart from "the guest is still running".
func TestDestroyEvidenceReportsAGuestItCannotObserve(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	fake.mu.Lock()
	fake.exists = false
	fake.mu.Unlock()

	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	_, err = destroyer.destroyProof(context.Background(), vm)
	if err == nil {
		t.Fatal("evidence was assembled for a guest that could not be observed")
	}
	if !strings.Contains(err.Error(), "observe unclaimed guest") {
		t.Fatalf("the unobservable guest was not named: %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("the hypervisor was mutated after a failed observation: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// A reservation claimed between the sweep and this attempt produces no
// evidence at all, even though the guest itself now looks perfectly safe to
// delete. The claim is checked here, inside the attempt, rather than trusted
// from the row the sweep read.
func TestDestroyEvidenceRefusesAReservationClaimedSinceTheSweep(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	fake.mu.Lock()
	fake.status = "stopped"
	fake.mu.Unlock()
	vm.ClaimedAt = claimedNow()

	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	_, err = destroyer.destroyProof(context.Background(), vm)
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("a claimed reservation produced evidence: %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("the hypervisor was mutated for a claimed reservation: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// The same guest, unclaimed, does produce evidence, so the refusal above is
// about the claim and not about the state of the guest.
func TestDestroyEvidenceIsAssembledForAStoppedUnclaimedGuest(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	fake.mu.Lock()
	fake.status = "stopped"
	fake.mu.Unlock()

	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	proof, err := destroyer.destroyProof(context.Background(), vm)
	if err != nil {
		t.Fatalf("a stopped unclaimed guest produced no evidence: %v", err)
	}
	if proof.ApprovalID != h.config.CleanupApprovalID {
		t.Fatalf("evidence carries approval %q, want the configured one", proof.ApprovalID)
	}
	if proof.ExpectedConfigDigest == "" {
		t.Fatal("evidence carries no digest read off the guest being deleted")
	}
	if !proof.Drained || !proof.NoActiveJob || !proof.RunnerDeregistered {
		t.Fatalf("evidence does not assert the unclaimed argument: %+v", proof)
	}
}
