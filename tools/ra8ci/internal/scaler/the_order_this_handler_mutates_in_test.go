// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A mutation is only ever asked for after the handler has satisfied itself
// that the durable VM is still the one the approvals describe and that the
// intent to mutate it is committed. These are the arms where one of those
// fails, and what matters in each is not the message but what did NOT happen
// afterwards.

// stalledLedger decides what committing an intent answers and leaves the rest
// of the ledger alone.
type stalledLedger struct {
	Ledger
	beginErr error
	beginOp  store.RunnerVMOperation
}

func (l stalledLedger) BeginRunnerVMOperation(context.Context, string, string, int64, string,
	store.RunnerVMSafetyEvidence) (store.RunnerVMOperation, error) {
	if l.beginErr != nil {
		return store.RunnerVMOperation{}, l.beginErr
	}
	return l.beginOp, nil
}

// approvedDrainingVM builds a durable VM that matches the harness's approvals exactly,
// so a test can break one field and know that field is why it was refused.
func approvedDrainingVM(t *testing.T, h *Handler, job string) store.RunnerVM {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	return store.RunnerVM{ID: id, Generation: 1, State: "draining", ExternalRunnerID: 77,
		ExternalRunnerName: "runner-9000",
		RunnerVMInput: store.RunnerVMInput{ScaleSetID: h.config.ScaleSetID, JobID: job, VMID: 9000,
			Node: h.config.Node, Pool: h.config.Pool, Storage: h.config.Storage, Name: "ra8-lab-ci-9000",
			TemplateVMID: h.config.TemplateVMID, TemplateName: h.config.TemplateName, TemplateDigest: h.config.TemplateDigest}}
}

// A VM whose durable identity has drifted from the approvals is refused before
// any intent is committed. Committing first would leave a record of an
// operation against a guest this scaler is no longer allowed to touch.
func TestAVMThatDriftedFromItsApprovalsIsRefusedBeforeIntentIsCommitted(t *testing.T) {
	handler, ledger, fake, _, job := testHarness(t)
	vm := approvedDrainingVM(t, handler, job.JobID)
	vm.VMID = 9100

	_, err := handler.execute(context.Background(), vm, "stop", store.RunnerVMSafetyEvidence{})
	if err == nil || err.Error() != "durable VM identity differs from current approvals" {
		t.Fatalf("answered %v, want the drifted identity refused", err)
	}
	ledger.mu.Lock()
	begins := ledger.beginCalls
	ledger.mu.Unlock()
	if begins != 0 {
		t.Fatalf("committed %d intents for a VM it cannot identify", begins)
	}
	if fake.stopCalls != 0 {
		t.Fatalf("issued %d stop requests for a VM it cannot identify", fake.stopCalls)
	}
}

// When the intent cannot be committed the mutation is not attempted. The order
// is the whole safety property: an external mutation with no durable record of
// the intent behind it is the one thing this handler must never do.
func TestAnUncommittedIntentStopsTheMutation(t *testing.T) {
	handler, ledger, fake, _, job := testHarness(t)
	refusal := errors.New("ledger unavailable")
	handler.ledger = stalledLedger{Ledger: ledger, beginErr: refusal}

	_, err := handler.execute(context.Background(), approvedDrainingVM(t, handler, job.JobID), "stop", store.RunnerVMSafetyEvidence{})
	if !errors.Is(err, refusal) {
		t.Fatalf("answered %v, want the ledger refusal carried back", err)
	}
	if fake.stopCalls != 0 {
		t.Fatalf("issued %d stop requests with no committed intent", fake.stopCalls)
	}
}

// An intent that was already issued is reconciled rather than re-issued. A
// second request would be a duplicate mutation against a guest whose first
// outcome is still unknown.
func TestAnIntentAlreadyIssuedIsReconciledRatherThanReissued(t *testing.T) {
	handler, ledger, fake, _, job := testHarness(t)
	vm := approvedDrainingVM(t, handler, job.JobID)
	handler.ledger = stalledLedger{Ledger: ledger, beginOp: store.RunnerVMOperation{ID: vm.ID, RunnerVMID: vm.ID,
		Kind: "stop", Generation: vm.Generation, PriorRequestIssued: true}}

	_, _ = handler.execute(context.Background(), vm, "stop", store.RunnerVMSafetyEvidence{})

	if fake.stopCalls != 0 {
		t.Fatalf("re-issued %d stop requests for an intent already on the wire", fake.stopCalls)
	}
}

// Reviewed cleanup re-checks the identity after the drain evidence is in hand,
// because the evidence says the runner is idle and says nothing about whether
// this is still an approved guest to destroy.
func TestReviewedCleanupRefusesADriftedIdentityAfterDraining(t *testing.T) {
	handler, _, _, _, job := testHarness(t)
	vm := approvedDrainingVM(t, handler, job.JobID)
	vm.Node = "pve-other"

	_, err := handler.drainEvidence(context.Background(), vm, completedJob(job), true)
	if err == nil || err.Error() != "durable VM identity differs from current approvals" {
		t.Fatalf("answered %v, want the drifted identity refused before destruction", err)
	}
}

// A guest the hypervisor cannot describe yields no cleanup evidence at all.
// The alternative would be destroying on the strength of a drain observation
// alone, with no current config digest to name what is being destroyed.
func TestReviewedCleanupCarriesBackAFailedGuestLookup(t *testing.T) {
	handler, _, fake, _, job := testHarness(t)
	fake.exists = false

	proof, err := handler.drainEvidence(context.Background(), approvedDrainingVM(t, handler, job.JobID), completedJob(job), true)
	if err == nil {
		t.Fatal("cleanup evidence was produced for a guest that could not be read")
	}
	if proof.ApprovalID != "" || proof.ExpectedConfigDigest != "" {
		t.Fatalf("a partial cleanup proof escaped: %+v", proof)
	}
}
