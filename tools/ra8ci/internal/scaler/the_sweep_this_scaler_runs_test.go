// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Reconcile is the sweep that revisits intents already committed to the
// ledger. It must never invent a mutation, must stop at the first reservation
// it cannot account for, and must name that reservation. The identity check
// underneath it is the fence that keeps a durable VM record from being driven
// after the approvals it was written under have changed.

// listedLedger answers the unresolved sweep from a list the test owns while
// leaving every other ledger method to the shared memory ledger.
type listedLedger struct {
	*memoryLedger
	pending  []store.RunnerVM
	failure  error
	listings int
	scaleSet int64
	limit    int
}

func (l *listedLedger) ListUnresolvedRunnerVMs(_ context.Context, scaleSetID int64, limit int) ([]store.RunnerVM, error) {
	l.listings++
	l.scaleSet = scaleSetID
	l.limit = limit
	if l.failure != nil {
		return nil, l.failure
	}
	return l.pending, nil
}

// approvedVM is a reservation that matches the harness's approvals exactly.
func approvedVM(id string) store.RunnerVM {
	return store.RunnerVM{
		ID: id,
		RunnerVMInput: store.RunnerVMInput{
			ScaleSetID: 42, VMID: 9000, Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
			TemplateVMID: 9001, TemplateName: "ra8-lab-template", TemplateDigest: testDigest,
			Name: "ra8-lab-ci-9000", JobID: "job-1", RunnerRequestID: 19, WorkflowRunID: 23,
		},
	}
}

func TestReconcileRefusesAnUnconfiguredScaler(t *testing.T) {
	var absent *Handler
	err := absent.Reconcile(context.Background(), github.Statistics{})
	if err == nil || !strings.Contains(err.Error(), "unconfigured scaler") {
		t.Fatalf("nil handler = %v", err)
	}
}

// A ledger that cannot be read is handed back as it came: the sweep must not
// dress a read failure up as a reconciliation outcome.
func TestReconcileHandsBackALedgerFailureUnchanged(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	unreadable := errors.New("unresolved reservations are unreadable")
	handler.ledger = &listedLedger{memoryLedger: ledger, failure: unreadable}
	if err := handler.Reconcile(context.Background(), github.Statistics{}); !errors.Is(err, unreadable) {
		t.Fatalf("reconcile = %v, want the ledger failure", err)
	}
}

// The sweep is asked for this scale set and this batch ceiling, and a
// reservation with nothing outstanding is walked past rather than mutated.
func TestReconcileAsksForItsOwnScaleSetAndLeavesSettledReservationsAlone(t *testing.T) {
	handler, ledger, fake, _, _ := testHarness(t)
	settled := approvedVM("01996f90-3415-7cfe-8ff1-600058131af0")
	second := approvedVM("01996f90-3415-7cfe-8ff1-600058131af1")
	listing := &listedLedger{memoryLedger: ledger, pending: []store.RunnerVM{settled, second}}
	handler.ledger = listing
	if err := handler.Reconcile(context.Background(), github.Statistics{}); err != nil {
		t.Fatalf("reconcile = %v", err)
	}
	if listing.listings != 1 || listing.scaleSet != 42 || listing.limit != handler.config.MaxReconcileBatch {
		t.Fatalf("sweep asked for scale set %d limit %d over %d listing(s)", listing.scaleSet, listing.limit, listing.listings)
	}
	if fake.cloneCalls != 0 || fake.startCalls != 0 || fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a settled reservation mutated Proxmox: clone=%d start=%d stop=%d delete=%d",
			fake.cloneCalls, fake.startCalls, fake.stopCalls, fake.deleteCalls)
	}
}

// A reservation the handler cannot account for stops the sweep and is named,
// so an operator knows which record to look at rather than which batch.
func TestReconcileNamesTheReservationItCouldNotAccountFor(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	foreign := approvedVM("01996f90-3415-7cfe-8ff1-600058131af2")
	foreign.Node = "someone-elses-node"
	foreign.UnknownOutcome = true
	foreign.CurrentOperationID = "01996f90-3415-7cfe-8ff1-600058131af3"
	handler.ledger = &listedLedger{memoryLedger: ledger, pending: []store.RunnerVM{foreign}}
	err := handler.Reconcile(context.Background(), github.Statistics{})
	if err == nil {
		t.Fatal("a reservation outside current approvals was reconciled")
	}
	if !strings.Contains(err.Error(), foreign.ID) {
		t.Fatalf("reconcile = %v, want the reservation ID named", err)
	}
	if !strings.Contains(err.Error(), "durable VM identity differs from current approvals") {
		t.Fatalf("reconcile = %v, want the identity refusal", err)
	}
}

// An outstanding reservation whose operation record is not fenced to it is
// refused rather than reconciled against whatever the operation says.
func TestAnOperationNotFencedToItsReservationIsRefused(t *testing.T) {
	handler, ledger, _, _, _ := testHarness(t)
	outstanding := approvedVM("01996f90-3415-7cfe-8ff1-600058131af4")
	outstanding.UnknownOutcome = true
	outstanding.CurrentOperationID = "01996f90-3415-7cfe-8ff1-600058131af5"
	// The memory ledger holds no operation under that ID, so the lookup
	// itself fails: the sweep reports it rather than proceeding.
	handler.ledger = &listedLedger{memoryLedger: ledger, pending: []store.RunnerVM{outstanding}}
	if err := handler.Reconcile(context.Background(), github.Statistics{}); err == nil {
		t.Fatal("an unfenced operation was reconciled")
	}
}

// identity() is the fence itself: every approval the reservation was written
// under has to still hold, one differing field is enough to refuse, and an
// approved reservation yields the identity the Proxmox client is driven with.
func TestADurableReservationIsDrivenOnlyUnderUnchangedApprovals(t *testing.T) {
	handler, _, _, _, _ := testHarness(t)
	approved := approvedVM("01996f90-3415-7cfe-8ff1-600058131af6")
	approved.CreationOperationID = "01996f90-3415-7cfe-8ff1-600058131af7"
	identity, err := handler.identity(approved)
	if err != nil {
		t.Fatalf("an approved reservation was refused: %v", err)
	}
	if identity.VMID != 9000 || identity.Node != "pve" || identity.Pool != "ra8-tf-lab" ||
		identity.Storage != "ra8-tf-lab" || identity.Name != "ra8-lab-ci-9000" ||
		identity.ReservationID != approved.ID || identity.CreationOperationID != approved.CreationOperationID {
		t.Fatalf("identity = %+v", identity)
	}

	changed := map[string]func(*store.RunnerVM){
		"scale set":       func(vm *store.RunnerVM) { vm.ScaleSetID = 43 },
		"node":            func(vm *store.RunnerVM) { vm.Node = "pve2" },
		"pool":            func(vm *store.RunnerVM) { vm.Pool = "other-pool" },
		"storage":         func(vm *store.RunnerVM) { vm.Storage = "other-storage" },
		"template VMID":   func(vm *store.RunnerVM) { vm.TemplateVMID = 9002 },
		"template name":   func(vm *store.RunnerVM) { vm.TemplateName = "other-template" },
		"template digest": func(vm *store.RunnerVM) { vm.TemplateDigest = strings.Repeat("b", 64) },
		"VMID":            func(vm *store.RunnerVM) { vm.VMID = 9100 },
	}
	for name, change := range changed {
		vm := approvedVM("01996f90-3415-7cfe-8ff1-600058131af8")
		change(&vm)
		if _, err := handler.identity(vm); err == nil {
			t.Fatalf("a reservation with a changed %s was accepted", name)
		}
	}
}

func TestOnlyAnApprovedVMIDIsRecognised(t *testing.T) {
	for name, held := range map[string]struct {
		ids  []int
		id   int
		want bool
	}{
		"the only approved ID":  {ids: []int{9000}, id: 9000, want: true},
		"one of several":        {ids: []int{9000, 9001, 9002}, id: 9002, want: true},
		"not approved":          {ids: []int{9000, 9001}, id: 9002, want: false},
		"no approvals at all":   {ids: nil, id: 9000, want: false},
		"a neighbouring number": {ids: []int{9000}, id: 9001, want: false},
	} {
		if got := containsID(held.ids, held.id); got != held.want {
			t.Fatalf("%s: containsID(%v, %d) = %v", name, held.ids, held.id, got)
		}
	}
}
