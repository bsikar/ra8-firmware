// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A resolution is the moment a committed mutation stops being unknown, so the
// paths here are the ones where it must NOT happen quietly: a result with no
// verifiable task behind it, a ledger that refuses the resolution, and a
// reconciliation that comes back saying the apply had no effect.

// refusingLedger lets one ledger call fail while the rest of the harness's
// ledger behaves normally.
type refusingLedger struct {
	Ledger
	resolveErr error
	upidErr    error
	// accepted, when set, stands in for a ledger that takes the resolution.
	// The shared in-memory ledger refuses any outcome other than "succeeded"
	// on the state-evidence path, so a failed apply cannot be resolved
	// through it without changing a fixture sixty tests depend on.
	accepted *store.RunnerVM
	proof    store.RunnerVMResolution
}

func (l *refusingLedger) ResolveRunnerVMOperation(ctx context.Context, actor, reservationID string,
	generation int64, operationID string, resolution store.RunnerVMResolution) (store.RunnerVM, error) {
	if l.resolveErr != nil {
		return store.RunnerVM{}, l.resolveErr
	}
	if l.accepted != nil {
		l.proof = resolution
		return *l.accepted, nil
	}
	return l.Ledger.ResolveRunnerVMOperation(ctx, actor, reservationID, generation, operationID, resolution)
}

func (l *refusingLedger) RecordRunnerVMUPID(ctx context.Context, actor, reservationID string,
	generation int64, operationID, upid string) error {
	if l.upidErr != nil {
		return l.upidErr
	}
	return l.Ledger.RecordRunnerVMUPID(ctx, actor, reservationID, generation, operationID, upid)
}

// A result that carries neither a task nor a clone marker leaves nothing to
// verify the mutation against. The operation stays unresolved and an operator
// is told to reconcile it, which is the recoverable half of the pair: a
// mutation resolved on nothing is not recoverable at all.
func TestAResultWithNoVerifiableTaskIsNotResolved(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	op := store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "proxmox", Generation: 3}

	for name, result := range map[string]proxmox.Result{
		"nothing at all":                  {},
		"a satisfied clone with no guest": {AlreadySatisfied: true},
		"a guest with no marker":          {VM: &proxmox.VM{}},
		"a satisfied stop":                {AlreadySatisfied: true, VM: &proxmox.VM{}},
	} {
		t.Run(name, func(t *testing.T) {
			kinded := op
			if name == "a satisfied stop" {
				kinded.Kind = "stop"
			}
			_, err := h.resolveVerified(context.Background(), store.RunnerVM{ID: "reservation", Generation: 3}, kinded, result)
			if err == nil || !strings.Contains(err.Error(), "operator reconciliation required") {
				t.Fatalf("answered %v, want an operator-reconciliation refusal", err)
			}
		})
	}
}

// A verified task the ledger cannot record is not treated as verified. The
// refusal says the UPID is not durable, because a task nobody can look up
// later is the same as no task at all.
func TestAVerifiedTaskThatIsNotDurableIsRefused(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	h.ledger = &refusingLedger{Ledger: h.ledger, upidErr: errors.New("write-ahead log is full")}
	op := store.RunnerVMOperation{ID: "operation", Kind: "clone", ProviderKind: "proxmox", Generation: 3}

	_, err := h.resolveVerified(context.Background(), store.RunnerVM{ID: "reservation", Generation: 3}, op,
		proxmox.Result{UPID: "UPID:pve:0000A1B2:00000000:00000000:qmclone:9000:ra8:"})
	if err == nil || !strings.Contains(err.Error(), "not durable") {
		t.Fatalf("answered %v, want a durability refusal", err)
	}
	if !strings.Contains(err.Error(), "write-ahead log is full") {
		t.Fatalf("the refusal dropped the ledger's own complaint: %v", err)
	}
}

// State evidence that says the apply failed still resolves the operation, and
// then reports the failure. Both halves matter: the ledger must stop carrying
// an unknown outcome, and the caller must not read the resolution as success.
func TestEvidenceOfAFailedApplyResolvesAndStillReports(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	settled := store.RunnerVM{ID: "reservation", Generation: 4, State: "reserved"}
	ledger := &refusingLedger{Ledger: h.ledger, accepted: &settled}
	h.ledger = ledger
	op := terraformOperation()
	op.Generation = 3
	evidence := &proxmox.TerraformEvidence{Outcome: "failed", PlanSHA256: planDigest,
		StateIdentitySHA256: stateDigest, ReconciliationSHA256: reconDigest, ObservedAt: time.Now()}

	resolved, err := h.resolveVerified(context.Background(),
		store.RunnerVM{ID: "reservation", Generation: 3}, op, proxmox.Result{TerraformEvidence: evidence})
	if err == nil || !strings.Contains(err.Error(), "no effect") {
		t.Fatalf("answered %v, want a no-effect report", err)
	}
	if resolved.Generation != settled.Generation {
		t.Fatalf("the operation was reported failed without being resolved: %+v", resolved)
	}
	if ledger.proof.Outcome != "failed" || ledger.proof.Source != "terraform_state" ||
		ledger.proof.ReconciliationSHA256 != reconDigest || !ledger.proof.PostStateVerified {
		t.Fatalf("the failure was not recorded as verified terraform state: %+v", ledger.proof)
	}
}

// When the ledger itself refuses the resolution, the refusal is handed back
// rather than swallowed, on both evidence paths. This is the case where the
// mutation really did happen and the record of it did not, which is the one
// an operator has to see.
func TestALedgerThatRefusesTheResolutionIsReported(t *testing.T) {
	lost := errors.New("reservation generation moved under the write")

	t.Run("on state evidence", func(t *testing.T) {
		h, _, _, _, _ := testHarness(t)
		h.ledger = &refusingLedger{Ledger: h.ledger, resolveErr: lost}
		op := terraformOperation()
		op.Generation = 3
		evidence := &proxmox.TerraformEvidence{Outcome: "succeeded", PlanSHA256: planDigest,
			StateIdentitySHA256: stateDigest, ReconciliationSHA256: reconDigest, ObservedAt: time.Now()}

		_, err := h.resolveVerified(context.Background(),
			store.RunnerVM{ID: "reservation", Generation: 3}, op, proxmox.Result{TerraformEvidence: evidence})
		if !errors.Is(err, lost) {
			t.Fatalf("answered %v, want the ledger's own refusal", err)
		}
	})

	t.Run("on preflight evidence", func(t *testing.T) {
		h, _, _, _, _ := testHarness(t)
		h.ledger = &refusingLedger{Ledger: h.ledger, resolveErr: lost}
		op := terraformOperation()
		op.Generation = 3

		_, err := h.resolveVerified(context.Background(), store.RunnerVM{ID: "reservation", Generation: 3}, op,
			proxmox.Result{TerraformPreflightNoEffect: &proxmox.TerraformPreflightNoEffect{
				PlanSHA256: planDigest, StateIdentitySHA256: stateDigest, ObservedAt: time.Now()}})
		if !errors.Is(err, lost) {
			t.Fatalf("answered %v, want the ledger's own refusal", err)
		}
	})
}
