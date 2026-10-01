// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A mutation is only resolved against evidence, and the evidence has to
// agree with the operation the ledger already committed. These hold the
// refusals, which all return before the ledger is asked to resolve
// anything: an operation left unresolved is recoverable, an operation
// resolved on evidence that does not match the plan is not.

const (
	planDigest  = "1f0c2a4b6d8e0f1a3b5c7d9e1f2a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c"
	stateDigest = "aa11bb22cc33dd44ee55ff6677889900aabbccddeeff00112233445566778899"
	reconDigest = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
)

func terraformOperation() store.RunnerVMOperation {
	return store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "terraform",
		PlanSHA256: planDigest, StateIdentitySHA256: stateDigest}
}

// The preflight proof says no apply was ever authorized, so anything that
// suggests an apply did start refuses it.
func TestPreflightEvidenceIsRefusedWhenAnApplyMayHaveStarted(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	started := time.Now().Add(-time.Minute)
	proof := func() *proxmox.TerraformPreflightNoEffect {
		return &proxmox.TerraformPreflightNoEffect{PlanSHA256: planDigest, StateIdentitySHA256: stateDigest, ObservedAt: time.Now()}
	}

	withState := terraformOperation()
	withUPID := terraformOperation()
	withUPID.UPID = "UPID:pve:0000A1B2:00000000:00000000:qmclone:9000:ra8:"
	applying := terraformOperation()
	applying.TerraformApplyStartedAt = &started
	unknownProvider := terraformOperation()
	unknownProvider.ProviderKind = "libvirt"
	noProvider := terraformOperation()
	noProvider.ProviderKind = ""

	unobserved := proof()
	unobserved.ObservedAt = time.Time{}
	ahead := proof()
	ahead.ObservedAt = time.Now().Add(5 * time.Second)
	old := proof()
	old.ObservedAt = time.Now().Add(-31 * time.Second)

	for name, answer := range map[string]struct {
		op     store.RunnerVMOperation
		result proxmox.Result
	}{
		"state evidence alongside the proof": {withState, proxmox.Result{TerraformPreflightNoEffect: proof(),
			TerraformEvidence: &proxmox.TerraformEvidence{Outcome: "failed"}}},
		"a task ID on the result":    {terraformOperation(), proxmox.Result{TerraformPreflightNoEffect: proof(), UPID: "UPID:pve:1:2:3:qmclone:9000:ra8:"}},
		"a task ID on the operation": {withUPID, proxmox.Result{TerraformPreflightNoEffect: proof()}},
		"an apply already started":   {applying, proxmox.Result{TerraformPreflightNoEffect: proof()}},
		"a provider we do not run":   {unknownProvider, proxmox.Result{TerraformPreflightNoEffect: proof()}},
		"no provider at all":         {noProvider, proxmox.Result{TerraformPreflightNoEffect: proof()}},
		"an unobserved proof":        {terraformOperation(), proxmox.Result{TerraformPreflightNoEffect: unobserved}},
		"a proof from the future":    {terraformOperation(), proxmox.Result{TerraformPreflightNoEffect: ahead}},
		"a proof half a minute old":  {terraformOperation(), proxmox.Result{TerraformPreflightNoEffect: old}},
	} {
		_, err := h.resolveVerified(t.Context(), store.RunnerVM{ID: "reservation"}, answer.op, answer.result)
		if err == nil || !strings.Contains(err.Error(), "invalid or stale Terraform preflight no-effect evidence") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// And the proof has to name the plan the operation was committed with. A
// Proxmox operation has no plan at all, so a proof carrying digests is as
// wrong as a Terraform proof carrying the wrong ones.
func TestPreflightEvidenceMustNameTheDurablePlan(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	observed := time.Now()
	hypervisor := store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "proxmox"}
	planned := terraformOperation()
	unplanned := terraformOperation()
	unplanned.PlanSHA256 = ""

	for name, answer := range map[string]struct {
		op    store.RunnerVMOperation
		proof proxmox.TerraformPreflightNoEffect
	}{
		"a hypervisor operation carrying a plan digest": {hypervisor, proxmox.TerraformPreflightNoEffect{PlanSHA256: planDigest, ObservedAt: observed}},
		"a hypervisor operation carrying a state identity": {hypervisor,
			proxmox.TerraformPreflightNoEffect{StateIdentitySHA256: stateDigest, ObservedAt: observed}},
		"a planned operation with no durable plan": {unplanned,
			proxmox.TerraformPreflightNoEffect{PlanSHA256: planDigest, StateIdentitySHA256: stateDigest, ObservedAt: observed}},
		"another plan": {planned,
			proxmox.TerraformPreflightNoEffect{PlanSHA256: reconDigest, StateIdentitySHA256: stateDigest, ObservedAt: observed}},
		"another state identity": {planned,
			proxmox.TerraformPreflightNoEffect{PlanSHA256: planDigest, StateIdentitySHA256: reconDigest, ObservedAt: observed}},
	} {
		proof := answer.proof
		_, err := h.resolveVerified(t.Context(), store.RunnerVM{ID: "reservation"}, answer.op,
			proxmox.Result{TerraformPreflightNoEffect: &proof})
		if err == nil || !strings.Contains(err.Error(), "does not match the durable plan") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A hypervisor operation whose proof carries nothing extra passes both
// guards, which is the pairing that proves the refusals above are about
// the mismatch and not about a Proxmox operation being refused outright.
func TestAHypervisorPreflightProofWithNoPlanPassesTheGuards(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	hypervisor := store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "proxmox"}
	proof := proxmox.TerraformPreflightNoEffect{ObservedAt: time.Now()}
	_, err := h.resolveVerified(t.Context(), store.RunnerVM{ID: "reservation"}, hypervisor,
		proxmox.Result{TerraformPreflightNoEffect: &proof})
	if err == nil {
		t.Fatal("the proof was accepted and the operation resolved, which this fixture cannot do")
	}
	if strings.Contains(err.Error(), "invalid or stale") || strings.Contains(err.Error(), "does not match the durable plan") {
		t.Fatalf("a clean hypervisor proof was refused by a guard: %v", err)
	}
}

// State evidence is the other half: it resolves an operation as succeeded
// or failed, so every digest it names is held to the plan and to the
// shape of a SHA-256.
func TestStateEvidenceIsHeldToThePlanAndTheClock(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	evidence := func() *proxmox.TerraformEvidence {
		return &proxmox.TerraformEvidence{Outcome: "succeeded", PlanSHA256: planDigest,
			StateIdentitySHA256: stateDigest, ReconciliationSHA256: reconDigest, ObservedAt: time.Now()}
	}
	hypervisor := store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "proxmox"}

	cancelled := evidence()
	cancelled.Outcome = "cancelled"
	unfinished := evidence()
	unfinished.Outcome = ""
	otherPlan := evidence()
	otherPlan.PlanSHA256 = reconDigest
	otherState := evidence()
	otherState.StateIdentitySHA256 = reconDigest
	shortRecon := evidence()
	shortRecon.ReconciliationSHA256 = reconDigest[:63]
	shoutedRecon := evidence()
	shoutedRecon.ReconciliationSHA256 = strings.ToUpper(reconDigest)
	noRecon := evidence()
	noRecon.ReconciliationSHA256 = ""
	unobserved := evidence()
	unobserved.ObservedAt = time.Time{}
	ahead := evidence()
	ahead.ObservedAt = time.Now().Add(5 * time.Second)
	old := evidence()
	old.ObservedAt = time.Now().Add(-31 * time.Second)

	for name, answer := range map[string]struct {
		op     store.RunnerVMOperation
		result proxmox.Result
	}{
		"a task ID alongside the evidence": {terraformOperation(), proxmox.Result{TerraformEvidence: evidence(),
			UPID: "UPID:pve:1:2:3:qmclone:9000:ra8:"}},
		"a cancelled outcome":        {terraformOperation(), proxmox.Result{TerraformEvidence: cancelled}},
		"no outcome at all":          {terraformOperation(), proxmox.Result{TerraformEvidence: unfinished}},
		"a hypervisor operation":     {hypervisor, proxmox.Result{TerraformEvidence: evidence()}},
		"another plan":               {terraformOperation(), proxmox.Result{TerraformEvidence: otherPlan}},
		"another state identity":     {terraformOperation(), proxmox.Result{TerraformEvidence: otherState}},
		"a truncated reconciliation": {terraformOperation(), proxmox.Result{TerraformEvidence: shortRecon}},
		"a shouted reconciliation":   {terraformOperation(), proxmox.Result{TerraformEvidence: shoutedRecon}},
		"no reconciliation":          {terraformOperation(), proxmox.Result{TerraformEvidence: noRecon}},
		"unobserved evidence":        {terraformOperation(), proxmox.Result{TerraformEvidence: unobserved}},
		"evidence from the future":   {terraformOperation(), proxmox.Result{TerraformEvidence: ahead}},
		"evidence half a minute old": {terraformOperation(), proxmox.Result{TerraformEvidence: old}},
	} {
		_, err := h.resolveVerified(t.Context(), store.RunnerVM{ID: "reservation"}, answer.op, answer.result)
		if err == nil || !strings.Contains(err.Error(), "invalid or stale Terraform state evidence") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}
