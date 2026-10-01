// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// Terraform's parsed state and the independent Proxmox observation beside it
// have to agree with BOTH the operation that was asked for and the outcome
// being claimed. The clone and destroy arms of that table are held through
// the resolutions in the_proofs_an_operation_is_resolved_on_test.go; this
// takes the start and stop arms, which are the ones where a failure still
// leaves a machine present and only its power state tells the two apart.

func aRunningOperation(kind string) RunnerVMOperation {
	applied := time.Now().Add(-time.Minute)
	return RunnerVMOperation{
		ID: runnerVMTestEvidence, Kind: kind, ProviderKind: "terraform",
		Status: "unresolved", Generation: 1,
		TerraformApplyStartedAt: &applied,
		PlanSHA256:              strings.Repeat("1", 64),
		StateIdentitySHA256:     strings.Repeat("2", 64),
	}
}

func anObservedState(now time.Time, operation RunnerVMOperation, outcome, status string) RunnerVMResolution {
	return RunnerVMResolution{
		Outcome: outcome, EvidenceID: runnerVMTestEvidence, Source: "terraform_state",
		ObservedAt: now.Add(-time.Second), PostStateVerified: true,
		PlanSHA256: operation.PlanSHA256, StateIdentitySHA256: operation.StateIdentitySHA256,
		ReconciliationSHA256: strings.Repeat("3", 64),
		TerraformStateHasVM:  true, TerraformVMAbsent: false, TerraformVMStatus: status,
	}
}

// Starting a machine succeeds into a running one and fails into a stopped
// one. Either way the machine is still in state, so the power word is the
// whole of the difference and the two cannot be swapped.
func TestStartingAMachineIsJudgedOnItsPowerState(t *testing.T) {
	now := time.Now()
	operation := aRunningOperation("start")

	if err := validateVMResolution(now, operation, anObservedState(now, operation, "succeeded", "running")); err != nil {
		t.Fatalf("a start that left a running machine was refused: %v", err)
	}
	if err := validateVMResolution(now, operation, anObservedState(now, operation, "failed", "stopped")); err != nil {
		t.Fatalf("a start that failed and left the machine stopped was refused: %v", err)
	}

	swapped := map[string]string{"succeeded": "stopped", "failed": "running"}
	for outcome, status := range swapped {
		if err := validateVMResolution(now, operation, anObservedState(now, operation, outcome, status)); !errors.Is(err, ErrDenied) {
			t.Fatalf("a start that %s reported %q and was accepted", outcome, status)
		}
	}
}

// Stopping is the mirror: it succeeds into a stopped machine and fails into a
// running one.
func TestStoppingAMachineIsJudgedOnItsPowerState(t *testing.T) {
	now := time.Now()
	operation := aRunningOperation("stop")

	if err := validateVMResolution(now, operation, anObservedState(now, operation, "succeeded", "stopped")); err != nil {
		t.Fatalf("a stop that left a stopped machine was refused: %v", err)
	}
	if err := validateVMResolution(now, operation, anObservedState(now, operation, "failed", "running")); err != nil {
		t.Fatalf("a stop that failed and left the machine running was refused: %v", err)
	}

	swapped := map[string]string{"succeeded": "running", "failed": "stopped"}
	for outcome, status := range swapped {
		if err := validateVMResolution(now, operation, anObservedState(now, operation, outcome, status)); !errors.Is(err, ErrDenied) {
			t.Fatalf("a stop that %s reported %q and was accepted", outcome, status)
		}
	}
}

// Neither a start nor a stop can be resolved by a machine that left state
// altogether: that shape belongs to a clone or a destroy, and reading it here
// would let a power operation be closed out by a machine nobody can find.
func TestAPowerOperationCannotBeResolvedByAnAbsentMachine(t *testing.T) {
	now := time.Now()

	for _, kind := range []string{"start", "stop"} {
		operation := aRunningOperation(kind)
		for _, outcome := range []string{"succeeded", "failed"} {
			absent := anObservedState(now, operation, outcome, "")
			absent.TerraformStateHasVM = false
			absent.TerraformVMAbsent = true
			if err := validateVMResolution(now, operation, absent); !errors.Is(err, ErrDenied) {
				t.Fatalf("a %s that %s was resolved by an absent machine", kind, outcome)
			}
		}
	}
}

// The state parsed out of Terraform and the status observed at the provider
// are two readings, and the gate wants them to agree: a machine the state
// says is there while the observation says it is gone resolves nothing, in
// either direction, for any operation.
func TestTheTwoReadingsOfAMachineHaveToAgree(t *testing.T) {
	now := time.Now()

	for _, kind := range []string{"clone", "start", "stop", "destroy"} {
		operation := aRunningOperation(kind)
		for _, outcome := range []string{"succeeded", "failed"} {
			bothTrue := anObservedState(now, operation, outcome, "stopped")
			bothTrue.TerraformVMAbsent = true
			if err := validateVMResolution(now, operation, bothTrue); !errors.Is(err, ErrDenied) {
				t.Fatalf("a %s took a machine both present and absent on %s", kind, outcome)
			}

			neither := anObservedState(now, operation, outcome, "stopped")
			neither.TerraformStateHasVM = false
			if err := validateVMResolution(now, operation, neither); !errors.Is(err, ErrDenied) {
				t.Fatalf("a %s took a machine neither in state nor absent on %s", kind, outcome)
			}
		}
	}
}

// An operation kind the table has never heard of resolves nothing, however
// well formed the rest of the proof is. That is what keeps a new kind from
// being silently resolvable before its arm is written.
func TestAnUnknownOperationKindResolvesNothing(t *testing.T) {
	now := time.Now()

	for _, kind := range []string{"", "reboot", "migrate", "CLONE", "resize"} {
		operation := aRunningOperation(kind)
		for _, status := range []string{"running", "stopped", ""} {
			proof := anObservedState(now, operation, "succeeded", status)
			if err := validateVMResolution(now, operation, proof); !errors.Is(err, ErrDenied) {
				t.Fatalf("a %q operation was resolved by a %q machine", kind, status)
			}
		}
	}
}

// A power word nobody uses is not a near miss to be tolerated: a machine
// reported as paused, suspended or shouted is refused rather than read as the
// closest state the table does know.
func TestAPowerWordNobodyUsesIsRefused(t *testing.T) {
	now := time.Now()

	for _, kind := range []string{"start", "stop"} {
		operation := aRunningOperation(kind)
		for _, status := range []string{"paused", "suspended", "RUNNING", "Stopped", "running ", "up"} {
			for _, outcome := range []string{"succeeded", "failed"} {
				if err := validateVMResolution(now, operation, anObservedState(now, operation, outcome, status)); !errors.Is(err, ErrDenied) {
					t.Fatalf("a %s took %q as a power state on %s", kind, status, outcome)
				}
			}
		}
	}
}
