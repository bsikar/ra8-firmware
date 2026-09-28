// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/proxmox"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A mutation whose outcome is unknown still leaves a task behind on the
// hypervisor, and the UPID naming that task is the only handle anybody has on
// it. When the UPID cannot be made durable the plane has lost its own
// mutation: the error has to say that, and nothing may be sent again.

// The clone request is answered, the task never finishes inside the operation
// budget, and the write that would record the UPID fails. The plane reports
// the lost handle rather than the timeout.
func TestAnUnknownMutationWhoseUPIDIsLostIsReportedAsLost(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.taskRunning = true
	ledger.loseUPIDOnce = true

	err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}})
	if err == nil {
		t.Fatal("a lost UPID under an unknown outcome was not reported")
	}
	if !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("error = %v, want the ledger failure carried back", err)
	}
	if !strings.Contains(err.Error(), "UPID not durable") {
		t.Fatalf("error = %v, want the lost handle named", err)
	}

	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 {
		t.Fatalf("clone was sent %d times, want once", fake.cloneCalls)
	}
}

// The lost handle is not reported as the unknown outcome itself. An operator
// told only "unknown outcome" would go looking for a task the ledger can name;
// here the ledger cannot, and that is the difference worth reading.
func TestALostUPIDDoesNotReadAsAnOrdinaryUnknownOutcome(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.taskRunning = true
	ledger.loseUPIDOnce = true
	message := github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}

	lost := h.Process(context.Background(), message)
	if lost == nil || errors.Is(lost, proxmox.ErrUnknownOutcome) {
		t.Fatalf("error = %v, want the lost handle distinguished from the unknown outcome", lost)
	}

	// The same shape with a ledger that keeps the UPID is the ordinary
	// unknown outcome, and reads as one.
	h2, _, fake2, _, job2 := testHarness(t)
	fake2.taskRunning = true
	kept := h2.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job2)}})
	if !errors.Is(kept, proxmox.ErrUnknownOutcome) {
		t.Fatalf("error = %v, want the ordinary unknown outcome", kept)
	}
	if lost.Error() == kept.Error() {
		t.Fatalf("both answers read %q", lost.Error())
	}
}

// Losing the UPID never licenses sending the lost mutation again. The task may
// well have run; cloning a second time would build a guest nobody reserved,
// and destroying is not the plane's to choose either.
func TestALostUPIDNeverLicensesASecondMutation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	fake.taskRunning = true
	ledger.loseUPIDOnce = true
	message := github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}

	if err := h.Process(context.Background(), message); err == nil {
		t.Fatal("a lost UPID was not reported")
	}
	if err := h.Process(context.Background(), message); err == nil {
		t.Fatal("a replay over a lost mutation was accepted")
	}

	vm, err := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if !vm.UnknownOutcome {
		t.Fatalf("the reservation forgot its unresolved mutation: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.deleteCalls != 0 {
		t.Fatalf("the lost mutation was sent again: clone=%d delete=%d",
			fake.cloneCalls, fake.deleteCalls)
	}
}
