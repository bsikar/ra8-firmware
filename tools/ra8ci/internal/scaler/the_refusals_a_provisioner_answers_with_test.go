// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// A hypervisor that refuses the clone leaves no guest behind, and the plane
// must not walk on to a start as though one existed.
func TestARefusedCloneNeverReachesAStart(t *testing.T) {
	h, ledger, fake, bootstrap, job := testHarness(t)
	fake.mu.Lock()
	fake.cloneRefused = true
	fake.mu.Unlock()

	err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}})
	if err == nil {
		t.Fatal("a refused clone was reported as an assignment that succeeded")
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 {
		t.Fatalf("clone asked %d times, want once and no retry", fake.cloneCalls)
	}
	if fake.startCalls != 0 {
		t.Fatalf("a guest that was never cloned was started %d times", fake.startCalls)
	}
	if bootstrap.calls != 0 {
		t.Fatalf("bootstrap was asked %d times for a guest that does not exist", bootstrap.calls)
	}
	if vm, err := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID); err == nil && vm.State == "running" {
		t.Fatalf("the ledger calls the reservation running after a refused clone: %+v", vm)
	}
}

// A clone that lands and a start that is refused stops before the JIT
// credential is minted: a runner credential handed to a guest that never
// booted is a credential nobody can account for.
func TestARefusedStartNeverMintsACredential(t *testing.T) {
	h, ledger, fake, bootstrap, job := testHarness(t)
	fake.mu.Lock()
	fake.startRefused = true
	fake.mu.Unlock()

	err := h.Process(context.Background(), github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}})
	if err == nil {
		t.Fatal("a refused start was reported as an assignment that succeeded")
	}
	fake.mu.Lock()
	cloned, started := fake.cloneCalls, fake.startCalls
	fake.mu.Unlock()
	if cloned != 1 || started != 1 {
		t.Fatalf("clone=%d start=%d, want one of each and no retry", cloned, started)
	}
	if bootstrap.calls != 0 {
		t.Fatalf("bootstrap minted %d times for a guest that never started", bootstrap.calls)
	}
	vm, err := ledger.GetRunnerVMByJob(context.Background(), 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if vm.State == "running" {
		t.Fatalf("the ledger calls the reservation running after a refused start: %+v", vm)
	}
}
