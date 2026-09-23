// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestTerraformStateEvidenceMustMatchDurablePlan(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	reservationID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	operationID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	planHash := strings.Repeat("a", 64)
	stateIdentityHash := strings.Repeat("b", 64)
	vm := store.RunnerVM{ID: reservationID, State: "cloning", Generation: 3,
		UnknownOutcome: true, CurrentOperationID: operationID}
	op := store.RunnerVMOperation{ID: operationID, RunnerVMID: reservationID,
		Kind: "clone", FromState: "reserved", PendingState: "cloning",
		Generation: 3, Status: "unresolved", ProviderKind: "terraform",
		PlanSHA256: planHash, StateIdentitySHA256: stateIdentityHash}
	ledger.mu.Lock()
	ledger.vm, ledger.op = vm, op
	ledger.mu.Unlock()

	evidence := &proxmox.TerraformEvidence{Outcome: "succeeded", PlanSHA256: planHash,
		StateIdentitySHA256: stateIdentityHash, ReconciliationSHA256: strings.Repeat("c", 64),
		ObservedAt: time.Now()}
	resolved, err := handler.resolveVerified(context.Background(), vm, op,
		proxmox.Result{TerraformEvidence: evidence})
	if err != nil {
		t.Fatal(err)
	}
	if resolved.State != "stopped" || resolved.UnknownOutcome || ledger.resolveCalls != 1 {
		t.Fatalf("Terraform evidence did not resolve the operation: %+v", resolved)
	}
}

func TestTerraformStateEvidenceCannotResolveAnotherPlan(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	reservationID, _ := store.NewID()
	operationID, _ := store.NewID()
	vm := store.RunnerVM{ID: reservationID, State: "cloning", Generation: 3,
		UnknownOutcome: true, CurrentOperationID: operationID}
	op := store.RunnerVMOperation{ID: operationID, RunnerVMID: reservationID,
		Kind: "clone", FromState: "reserved", PendingState: "cloning",
		Generation: 3, Status: "unresolved", ProviderKind: "terraform",
		PlanSHA256: strings.Repeat("a", 64), StateIdentitySHA256: strings.Repeat("b", 64)}
	ledger.mu.Lock()
	ledger.vm, ledger.op = vm, op
	ledger.mu.Unlock()

	evidence := &proxmox.TerraformEvidence{Outcome: "succeeded",
		PlanSHA256: strings.Repeat("d", 64), StateIdentitySHA256: op.StateIdentitySHA256,
		ReconciliationSHA256: strings.Repeat("c", 64), ObservedAt: time.Now()}
	if _, err := handler.resolveVerified(context.Background(), vm, op,
		proxmox.Result{TerraformEvidence: evidence}); err == nil {
		t.Fatal("accepted state evidence for a different saved plan")
	}
	if ledger.resolveCalls != 0 {
		t.Fatal("mismatched Terraform evidence reached the ledger")
	}
}

func TestTerraformPreflightNoEffectClosesOnlyUnappliedOperation(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	reservationID, _ := store.NewID()
	operationID, _ := store.NewID()
	vm := store.RunnerVM{ID: reservationID, State: "cloning", Generation: 3,
		UnknownOutcome: true, CurrentOperationID: operationID}
	op := store.RunnerVMOperation{ID: operationID, RunnerVMID: reservationID,
		Kind: "clone", FromState: "reserved", PendingState: "cloning",
		Generation: 3, Status: "unresolved", ProviderKind: "proxmox"}
	ledger.mu.Lock()
	ledger.vm, ledger.op = vm, op
	ledger.mu.Unlock()

	resolved, err := handler.resolveVerified(context.Background(), vm, op, proxmox.Result{
		TerraformPreflightNoEffect: &proxmox.TerraformPreflightNoEffect{ObservedAt: time.Now()},
	})
	if err == nil || resolved.State != "reserved" || ledger.resolveCalls != 1 {
		t.Fatalf("pre-apply operation was not closed as a verified no-op: vm=%+v calls=%d err=%v",
			resolved, ledger.resolveCalls, err)
	}
}
