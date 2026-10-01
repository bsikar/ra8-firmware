// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A completion can arrive over a row whose last mutation was never resolved:
// the hypervisor was asked to do something and nobody heard the answer. The
// completion does not step over that. It reconciles first, and what it can
// establish about the unresolved operation decides whether the teardown runs
// at all.

// A row carrying an unknown outcome and an operation that is not fenced to
// it is exactly the case where the plane must not guess: the completion is
// refused and no guest is touched.
func TestACompletionOverAnUnfencedUnknownOutcomeTearsNothingDown(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.CleanupRequested = true
		row.State = "draining"
		row.UnknownOutcome = true
		row.CurrentOperationID = "0f0e0d0c-0000-4000-8000-00000000abcd"
	})

	err := h.completed(context.Background(), completedJob(job))
	if err == nil {
		t.Fatal("a completion walked past an unresolved mutation")
	}

	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("an unresolved mutation was torn down anyway: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// An unknown outcome with no operation recorded against it has nothing to
// reconcile, so the completion carries on and judges the row on its state.
// A row already stopped and awaiting cleanup still has to earn its destroy:
// the final teardown asks for drain evidence, and without it the guest is
// left alone rather than deleted on the strength of a state field.
func TestACompletionReconcilesAnEmptyUnknownOutcomeAndStillAsksForEvidence(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.CleanupRequested = true
		row.State = "stopped"
		row.UnknownOutcome = true
		row.CurrentOperationID = ""
	})

	err := h.completed(context.Background(), completedJob(job))
	if err == nil {
		t.Fatal("a stopped row was destroyed without drain evidence")
	}
	if strings.Contains(err.Error(), "fenced") {
		t.Fatalf("nothing was recorded to reconcile, yet the completion refused on it: %v", err)
	}
	if !strings.Contains(err.Error(), "evidence") {
		t.Fatalf("error = %v, want the missing drain evidence named", err)
	}

	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State == "released" {
		t.Fatalf("the reservation was released without evidence: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.deleteCalls != 0 {
		t.Fatalf("the guest was destroyed %d times without evidence", fake.deleteCalls)
	}
}

// The refusal for an unfenced operation says so in as many words. It is the
// difference between a reservation an operator must go and resolve by hand
// and one the plane simply could not read.
func TestAnUnfencedOperationIsNamedAsSuchRatherThanQuotedAsAState(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowAs(t, ledger, func(row *store.RunnerVM) {
		row.CleanupRequested = true
		row.State = "draining"
		row.UnknownOutcome = true
		row.CurrentOperationID = "0f0e0d0c-0000-4000-8000-00000000abcd"
	})

	err := h.completed(context.Background(), completedJob(job))
	if err == nil || strings.Contains(err.Error(), "unexpected VM state") {
		t.Fatalf("error = %v, want the unresolved operation named rather than the state quoted", err)
	}
}
