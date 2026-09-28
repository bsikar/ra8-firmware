// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// An assignment can arrive for a reservation at any point in its life: the
// forge replays, and a message that was in flight when the guest moved on
// lands afterwards. So the step is a decision about the state it finds, and
// the states it does nothing for matter as much as the ones it acts on. Each
// case here also asserts the hypervisor was never asked, because an
// assignment that quietly clones or starts a second guest is the failure
// worth catching.

// rowInState puts the ledger's one reservation in state, leaving everything
// else the fixture set. The row is what the step reads, so this is the whole
// setup for a state decision.
func rowInState(t *testing.T, ledger *memoryLedger, state string) {
	t.Helper()
	ledger.mu.Lock()
	defer ledger.mu.Unlock()
	ledger.vm.State = state
}

func hypervisorCalls(fake *fakeProxmox) (int, int) {
	fake.mu.Lock()
	defer fake.mu.Unlock()
	return fake.cloneCalls, fake.startCalls
}

// Registered and draining are the two states where the runner already has the
// job. There is nothing for an assignment to do and nothing wrong either, so
// it is accepted rather than refused.
func TestAnAssignmentForARunnerThatAlreadyHasTheJobIsAcceptedAndDoesNothing(t *testing.T) {
	for _, state := range []string{"registered", "draining"} {
		h, ledger, fake, _, job := testHarness(t)
		expired(t, h, ledger, job)
		rowInState(t, ledger, state)
		clones, starts := hypervisorCalls(fake)
		if err := h.assigned(context.Background(), assignedJob(job)); err != nil {
			t.Fatalf("an assignment for a %s reservation = %v", state, err)
		}
		if nowClones, nowStarts := hypervisorCalls(fake); nowClones != clones || nowStarts != starts {
			t.Fatalf("a %s reservation still asked the hypervisor: clone %d->%d, start %d->%d",
				state, clones, nowClones, starts, nowStarts)
		}
	}
}

// A state the step has no rule for is refused by name. Naming it is the
// point: the operator reading the error learns which row to go and look at,
// and a silent no-op here would leave a job with no guest and no complaint.
func TestAnAssignmentForAReservationInAnUnhandledStateNamesThatState(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowInState(t, ledger, "quarantined")
	clones, starts := hypervisorCalls(fake)
	err := h.assigned(context.Background(), assignedJob(job))
	if err == nil {
		t.Fatal("an assignment for an unhandled state was accepted")
	}
	if !strings.Contains(err.Error(), `unexpected VM state "quarantined"`) {
		t.Fatalf("the refusal does not name the state it found: %v", err)
	}
	if nowClones, nowStarts := hypervisorCalls(fake); nowClones != clones || nowStarts != starts {
		t.Fatalf("a refused assignment still asked the hypervisor: clone %d->%d, start %d->%d",
			clones, nowClones, starts, nowStarts)
	}
}

// A released reservation, and one already on its way out, are both finished
// business. A replayed assignment must not walk either of them back into a
// running guest.
func TestALateAssignmentNeverUndoesAFinishedReservation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowInState(t, ledger, "released")
	clones, starts := hypervisorCalls(fake)
	if err := h.assigned(context.Background(), assignedJob(job)); err != nil {
		t.Fatalf("a late assignment for a released reservation = %v", err)
	}

	ledger.mu.Lock()
	ledger.vm.State = "reserved"
	ledger.vm.CleanupRequested = true
	ledger.mu.Unlock()
	if err := h.assigned(context.Background(), assignedJob(job)); err != nil {
		t.Fatalf("a late assignment for a reservation being cleaned up = %v", err)
	}
	if nowClones, nowStarts := hypervisorCalls(fake); nowClones != clones || nowStarts != starts {
		t.Fatalf("a finished reservation was relaunched: clone %d->%d, start %d->%d",
			clones, nowClones, starts, nowStarts)
	}
}

// The gate is asked before a stopped reservation is started, not only before
// a reserved one is cloned. Restarting a guest that already exists is still
// new capacity, and the refusal names the gate rather than the guest.
func TestAStoppedReservationIsNotStartedWhileTheBackupGateRefuses(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	rowInState(t, ledger, "stopped")
	h.backup = testBackupGate{err: errors.New("no full backup inside the window")}
	clones, starts := hypervisorCalls(fake)
	err := h.assigned(context.Background(), assignedJob(job))
	if err == nil {
		t.Fatal("a stopped reservation was started with the backup gate refusing")
	}
	if !strings.Contains(err.Error(), "off-VM backup gate before VM launch") {
		t.Fatalf("the refusal does not name the gate: %v", err)
	}
	if nowClones, nowStarts := hypervisorCalls(fake); nowClones != clones || nowStarts != starts {
		t.Fatalf("a gated assignment still asked the hypervisor: clone %d->%d, start %d->%d",
			clones, nowClones, starts, nowStarts)
	}
}

// The guest identity is derived from the row and checked against the VMIDs
// this handler was configured for. A row naming a VMID outside that set is
// refused before the hypervisor is asked anything at all, which is what
// keeps one scale set from starting another's guest.
func TestAStartForAGuestOutsideTheConfiguredVMIDsAsksTheHypervisorNothing(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	vm.VMID = 4242
	clones, starts := hypervisorCalls(fake)
	if err := h.prepareAndStart(context.Background(), vm); err == nil {
		t.Fatal("a guest outside the configured VMIDs was started")
	}
	if nowClones, nowStarts := hypervisorCalls(fake); nowClones != clones || nowStarts != starts {
		t.Fatalf("a refused start still asked the hypervisor: clone %d->%d, start %d->%d",
			clones, nowClones, starts, nowStarts)
	}
}

// An event in the assigned bucket that is not an assignment is refused
// on its kind, before any row is read.
func TestAnAssignmentStepRefusesAMessageOfAnotherKind(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	for _, wrong := range []github.Job{job, completedJob(job)} {
		if err := h.assigned(context.Background(), wrong); err == nil ||
			!strings.Contains(err.Error(), "not a job-assigned message") {
			t.Fatalf("a %q event = %v", wrong.Kind, err)
		}
	}
}
