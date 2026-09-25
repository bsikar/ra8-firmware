// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"testing"

	"github.com/actions/scaleset"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

func TestProvesJobAssignedAcceptsOnlyTheAssignedKind(t *testing.T) {
	for _, kind := range everyJobEventKind {
		job := github.Job{Kind: kind}
		want := kind == scaleset.MessageTypeJobAssigned
		if got := provesJobAssigned(job); got != want {
			t.Fatalf("kind %q: provesJobAssigned=%v want %v", kind, got, want)
		}
	}
}

func TestAnAssignedEventStillReservesTheGuest(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}); err != nil {
		t.Fatal(err)
	}
	vm, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if vm.State != "running" {
		t.Fatalf("assigned event did not bring up the guest: %+v", vm)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 1 || fake.startCalls != 1 {
		t.Fatalf("assigned event mutation count clone=%d start=%d", fake.cloneCalls, fake.startCalls)
	}
}

func TestAMisfiledEventInTheAssignedBucketNeverReservesAGuest(t *testing.T) {
	for _, kind := range everyJobEventKind {
		if kind == scaleset.MessageTypeJobAssigned {
			continue
		}
		h, ledger, fake, _, job := testHarness(t)
		misfiled := job
		misfiled.Kind = kind
		ctx := context.Background()
		if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{misfiled}}); err == nil {
			t.Fatalf("kind %q was accepted as an assignment", kind)
		}
		if _, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID); err == nil {
			t.Fatalf("kind %q reserved a guest", kind)
		}
		fake.mu.Lock()
		if fake.cloneCalls != 0 || fake.startCalls != 0 {
			fake.mu.Unlock()
			t.Fatalf("kind %q reached the hypervisor", kind)
		}
		fake.mu.Unlock()
	}
}
