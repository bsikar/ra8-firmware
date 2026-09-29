// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// An operation against a lab machine is committed as an intent before the
// provider is asked to do anything, so it has to be resolved afterwards with
// proof of what actually happened. validateVMResolution is the gate on that
// proof: five sources, each with its own conditions, and a shared set of
// requirements none of them can skip. All of it is pure, so none of it needs
// the lab or a database.

func aResolvedOperation(kind, provider string) RunnerVMOperation {
	return RunnerVMOperation{
		ID: runnerVMTestEvidence, Kind: kind, ProviderKind: provider,
		Status: "unresolved", Generation: 1,
	}
}

func aResolution(now time.Time, source string) RunnerVMResolution {
	return RunnerVMResolution{
		Outcome: "succeeded", EvidenceID: runnerVMTestEvidence, Source: source,
		ObservedAt: now.Add(-time.Second), PostStateVerified: true,
	}
}

// Whatever the source, the proof has to say what happened, carry a real
// evidence identifier, state that the machine's state after the fact was
// checked, and be recent. These are asked before the source is even read.
func TestEveryResolutionIsRecentAndVerified(t *testing.T) {
	now := time.Now()
	operation := aResolvedOperation("clone", "proxmox")
	operation.UPID = "UPID:lab-1:0000A1B2::qmclone:"

	if err := validateVMResolution(now, operation, aResolution(now, "upid")); err != nil {
		t.Fatalf("a whole proof was refused: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMResolution){
		"no outcome":             func(p *RunnerVMResolution) { p.Outcome = "" },
		"an outcome nobody has":  func(p *RunnerVMResolution) { p.Outcome = "cancelled" },
		"a shouted outcome":      func(p *RunnerVMResolution) { p.Outcome = "SUCCEEDED" },
		"no evidence ID":         func(p *RunnerVMResolution) { p.EvidenceID = "" },
		"an evidence ID by eye":  func(p *RunnerVMResolution) { p.EvidenceID = "evidence-1" },
		"the state unverified":   func(p *RunnerVMResolution) { p.PostStateVerified = false },
		"never observed":         func(p *RunnerVMResolution) { p.ObservedAt = time.Time{} },
		"observed in the future": func(p *RunnerVMResolution) { p.ObservedAt = now.Add(2 * time.Second) },
		"observed a minute ago":  func(p *RunnerVMResolution) { p.ObservedAt = now.Add(-31 * time.Second) },
		"a source nobody has":    func(p *RunnerVMResolution) { p.Source = "trust-me" },
		"no source at all":       func(p *RunnerVMResolution) { p.Source = "" },
	} {
		proof := aResolution(now, "upid")
		bend(&proof)
		if err := validateVMResolution(now, operation, proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("a proof with %s answered %v, want denied", name, err)
		}
	}
}

// The freshness window is wider here than the ten seconds stopping a machine
// is held to, because a resolution is read after a provider call rather than
// before one. Pinned at both edges so neither is read as the other.
func TestAResolutionWindowIsThirtySeconds(t *testing.T) {
	now := time.Now()
	operation := aResolvedOperation("clone", "proxmox")
	operation.UPID = "UPID:lab-1:0000A1B2::qmclone:"

	atTheEdge := aResolution(now, "upid")
	atTheEdge.ObservedAt = now.Add(-30 * time.Second)
	if err := validateVMResolution(now, operation, atTheEdge); err != nil {
		t.Fatalf("a reading exactly thirty seconds old was refused: %v", err)
	}

	pastTheEdge := atTheEdge
	pastTheEdge.ObservedAt = now.Add(-30*time.Second - time.Millisecond)
	if err := validateVMResolution(now, operation, pastTheEdge); !errors.Is(err, ErrDenied) {
		t.Fatalf("a reading past the window was accepted: %v", err)
	}

	justAhead := atTheEdge
	justAhead.ObservedAt = now.Add(time.Second)
	if err := validateVMResolution(now, operation, justAhead); err != nil {
		t.Fatalf("a reading inside the clock tolerance was refused: %v", err)
	}
}

// A Proxmox task ID is proof only when the operation actually went to Proxmox
// and actually recorded one. A clone marker is proof only for a clone.
func TestAProviderProofMustMatchTheProviderItCameFrom(t *testing.T) {
	now := time.Now()

	withUPID := aResolvedOperation("clone", "proxmox")
	withUPID.UPID = "UPID:lab-1:0000A1B2::qmclone:"
	if err := validateVMResolution(now, withUPID, aResolution(now, "upid")); err != nil {
		t.Fatalf("a Proxmox task ID was refused: %v", err)
	}
	if err := validateVMResolution(now, withUPID, aResolution(now, "clone_marker")); err != nil {
		t.Fatalf("a clone marker was refused for a clone: %v", err)
	}

	for name, operation := range map[string]RunnerVMOperation{
		"an operation that recorded no task": aResolvedOperation("clone", "proxmox"),
		"an operation that went elsewhere": func() RunnerVMOperation {
			o := aResolvedOperation("clone", "terraform")
			o.UPID = "UPID:lab-1::"
			return o
		}(),
	} {
		if err := validateVMResolution(now, operation, aResolution(now, "upid")); !errors.Is(err, ErrDenied) {
			t.Fatalf("a task ID from %s was accepted", name)
		}
	}

	for _, kind := range []string{"start", "stop", "destroy"} {
		operation := aResolvedOperation(kind, "proxmox")
		if err := validateVMResolution(now, operation, aResolution(now, "clone_marker")); !errors.Is(err, ErrDenied) {
			t.Fatalf("a clone marker resolved a %s", kind)
		}
	}
}

// An operator can resolve anything, which is the escape hatch for a machine
// the automation lost track of. The price is that the approval has to be a
// real identifier rather than a note.
func TestAnOperatorNeedsARealApproval(t *testing.T) {
	now := time.Now()

	for _, kind := range []string{"clone", "start", "stop", "destroy"} {
		for _, provider := range []string{"proxmox", "terraform"} {
			proof := aResolution(now, "operator")
			proof.OperatorApprovalID = runnerVMTestApproval
			if err := validateVMResolution(now, aResolvedOperation(kind, provider), proof); err != nil {
				t.Fatalf("an approved %s on %s was refused: %v", kind, provider, err)
			}
		}
	}

	for name, approval := range map[string]string{
		"no approval":                  "",
		"an approval by eye":           "approved-by-me",
		"an approval of another shape": "01996f90-3415-4cfe-8ff1-600058131aff",
	} {
		proof := aResolution(now, "operator")
		proof.OperatorApprovalID = approval
		if err := validateVMResolution(now, aResolvedOperation("destroy", "proxmox"), proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("an operator resolution with %s was accepted", name)
		}
	}
}

func aTerraformOperation() RunnerVMOperation {
	applied := time.Now().Add(-time.Minute)
	operation := aResolvedOperation("clone", "terraform")
	operation.TerraformApplyStartedAt = &applied
	operation.PlanSHA256 = strings.Repeat("1", 64)
	operation.StateIdentitySHA256 = strings.Repeat("2", 64)
	return operation
}

func aTerraformResolution(now time.Time, operation RunnerVMOperation) RunnerVMResolution {
	proof := aResolution(now, "terraform_state")
	proof.PlanSHA256 = operation.PlanSHA256
	proof.StateIdentitySHA256 = operation.StateIdentitySHA256
	proof.ReconciliationSHA256 = strings.Repeat("3", 64)
	proof.TerraformStateHasVM = true
	proof.TerraformVMStatus = "stopped"
	return proof
}

// Terraform state is proof only when the apply it describes is the apply this
// operation started, which is what binding the plan and state identities
// does: a reading from another apply cannot resolve this one.
func TestTerraformStateMustBeTheApplyThisOperationStarted(t *testing.T) {
	now := time.Now()
	operation := aTerraformOperation()

	if err := validateVMResolution(now, operation, aTerraformResolution(now, operation)); err != nil {
		t.Fatalf("a bound Terraform reading was refused: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMOperation, *RunnerVMResolution){
		"an operation that went to Proxmox": func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.ProviderKind = "proxmox" },
		"an apply that never started":       func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.TerraformApplyStartedAt = nil },
		"another plan":                      func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.PlanSHA256 = strings.Repeat("9", 64) },
		"another state identity":            func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.StateIdentitySHA256 = strings.Repeat("9", 64) },
		"no reconciliation digest":          func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.ReconciliationSHA256 = "" },
		"a reconciliation digest by eye":    func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.ReconciliationSHA256 = "state-is-fine" },
		"a shouted reconciliation digest":   func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.ReconciliationSHA256 = strings.Repeat("A", 64) },
	} {
		bentOperation := aTerraformOperation()
		proof := aTerraformResolution(now, bentOperation)
		bend(&bentOperation, &proof)
		if err := validateVMResolution(now, bentOperation, proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("a Terraform reading with %s was accepted", name)
		}
	}
}

// The state Terraform parsed and the machine Proxmox observed have to agree
// with the operation AND its outcome. A clone that succeeded leaves a stopped
// machine in state; a clone that failed leaves nothing at all. Claiming a
// success while the machine is absent is the case this refuses.
func TestTerraformStateMustAgreeWithWhatWasAsked(t *testing.T) {
	now := time.Now()
	operation := aTerraformOperation()

	succeededButAbsent := aTerraformResolution(now, operation)
	succeededButAbsent.TerraformStateHasVM = false
	succeededButAbsent.TerraformVMAbsent = true
	succeededButAbsent.TerraformVMStatus = ""
	if err := validateVMResolution(now, operation, succeededButAbsent); !errors.Is(err, ErrDenied) {
		t.Fatalf("a clone succeeded into an absent machine: %v", err)
	}

	failedAndAbsent := succeededButAbsent
	failedAndAbsent.Outcome = "failed"
	if err := validateVMResolution(now, operation, failedAndAbsent); err != nil {
		t.Fatalf("a clone that failed and left nothing was refused: %v", err)
	}

	destroy := aTerraformOperation()
	destroy.Kind = "destroy"
	destroyed := aTerraformResolution(now, destroy)
	destroyed.TerraformStateHasVM = false
	destroyed.TerraformVMAbsent = true
	destroyed.TerraformVMStatus = ""
	if err := validateVMResolution(now, destroy, destroyed); err != nil {
		t.Fatalf("a destroy that left nothing was refused: %v", err)
	}
	stillThere := aTerraformResolution(now, destroy)
	if err := validateVMResolution(now, destroy, stillThere); !errors.Is(err, ErrDenied) {
		t.Fatalf("a destroy succeeded with the machine still in state: %v", err)
	}
}

// A preflight failure is the one source that resolves an operation the
// provider was never actually asked to perform, so it can only ever report a
// failure, and only while nothing was issued: no task ID, no apply started,
// and no reconciliation digest, because there is no state to reconcile.
func TestAPreflightFailureOnlyResolvesWhatWasNeverIssued(t *testing.T) {
	now := time.Now()

	failed := aResolution(now, "terraform_preflight")
	failed.Outcome = "failed"
	if err := validateVMResolution(now, aResolvedOperation("clone", "proxmox"), failed); err != nil {
		t.Fatalf("a Proxmox preflight failure was refused: %v", err)
	}

	succeeded := failed
	succeeded.Outcome = "succeeded"
	if err := validateVMResolution(now, aResolvedOperation("clone", "proxmox"), succeeded); !errors.Is(err, ErrDenied) {
		t.Fatalf("a preflight reported a success: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMOperation, *RunnerVMResolution){
		"a provider nobody has":       func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.ProviderKind = "libvirt" },
		"a task already issued":       func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.UPID = "UPID:lab-1::" },
		"a plan on a Proxmox op":      func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.PlanSHA256 = strings.Repeat("1", 64) },
		"a plan claimed in the proof": func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.PlanSHA256 = strings.Repeat("1", 64) },
		"a state identity claimed":    func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.StateIdentitySHA256 = strings.Repeat("2", 64) },
		"a reconciliation digest":     func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.ReconciliationSHA256 = strings.Repeat("3", 64) },
	} {
		operation := aResolvedOperation("clone", "proxmox")
		proof := aResolution(now, "terraform_preflight")
		proof.Outcome = "failed"
		bend(&operation, &proof)
		if err := validateVMResolution(now, operation, proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("a preflight failure with %s was accepted", name)
		}
	}
}

// A Terraform preflight failure is held to the mirror of the Proxmox one: the
// plan it names must be the plan the operation prepared, because a preflight
// that failed after planning still has an identity to bind to.
func TestATerraformPreflightNamesThePlanItPrepared(t *testing.T) {
	now := time.Now()

	operation := aResolvedOperation("clone", "terraform")
	operation.PlanSHA256 = strings.Repeat("1", 64)
	operation.StateIdentitySHA256 = strings.Repeat("2", 64)

	proof := aResolution(now, "terraform_preflight")
	proof.Outcome = "failed"
	proof.PlanSHA256 = operation.PlanSHA256
	proof.StateIdentitySHA256 = operation.StateIdentitySHA256
	if err := validateVMResolution(now, operation, proof); err != nil {
		t.Fatalf("a bound Terraform preflight failure was refused: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMOperation, *RunnerVMResolution){
		"an operation that never planned": func(o *RunnerVMOperation, _ *RunnerVMResolution) { o.PlanSHA256 = "" },
		"another plan":                    func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.PlanSHA256 = strings.Repeat("9", 64) },
		"another state identity":          func(_ *RunnerVMOperation, p *RunnerVMResolution) { p.StateIdentitySHA256 = strings.Repeat("9", 64) },
		"an apply already started": func(o *RunnerVMOperation, _ *RunnerVMResolution) {
			applied := time.Now()
			o.TerraformApplyStartedAt = &applied
		},
	} {
		bentOperation := aResolvedOperation("clone", "terraform")
		bentOperation.PlanSHA256 = strings.Repeat("1", 64)
		bentOperation.StateIdentitySHA256 = strings.Repeat("2", 64)
		bentProof := aResolution(now, "terraform_preflight")
		bentProof.Outcome = "failed"
		bentProof.PlanSHA256 = bentOperation.PlanSHA256
		bentProof.StateIdentitySHA256 = bentOperation.StateIdentitySHA256
		bend(&bentOperation, &bentProof)
		if err := validateVMResolution(now, bentOperation, bentProof); !errors.Is(err, ErrDenied) {
			t.Fatalf("a Terraform preflight with %s was accepted", name)
		}
	}
}
