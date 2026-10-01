// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"testing"

	"github.com/actions/scaleset"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

var everyJobEventKind = []scaleset.MessageType{
	"",
	scaleset.MessageTypeJobAvailable,
	scaleset.MessageTypeJobAssigned,
	scaleset.MessageTypeJobStarted,
	scaleset.MessageTypeJobCompleted,
}

func TestOnlyTheStartedKindProvesARunnerStarted(t *testing.T) {
	for _, kind := range everyJobEventKind {
		want := kind == scaleset.MessageTypeJobStarted
		if got := provesRunnerStarted(github.Job{Kind: kind}); got != want {
			t.Fatalf("provesRunnerStarted(%q)=%v, want %v", kind, got, want)
		}
	}
}

func TestOnlyTheCompletedKindProvesAJobCompleted(t *testing.T) {
	for _, kind := range everyJobEventKind {
		want := kind == scaleset.MessageTypeJobCompleted
		if got := provesJobCompleted(github.Job{Kind: kind}); got != want {
			t.Fatalf("provesJobCompleted(%q)=%v, want %v", kind, got, want)
		}
	}
}

func TestRegistrationEvidenceRefusesEveryKindButStarted(t *testing.T) {
	for _, kind := range everyJobEventKind {
		if kind == scaleset.MessageTypeJobStarted {
			continue
		}
		observer, admin, vm, job := observerFixture(t)
		job.Kind = kind
		evidence, err := observer.Registered(context.Background(), vm, job)
		if err == nil || evidence.EvidenceID != "" {
			t.Fatalf("kind %q backed registration evidence: %+v %v", kind, evidence, err)
		}
		if admin.calls != 0 {
			t.Fatalf("kind %q reached GitHub before the event kind was judged", kind)
		}
	}
}

func TestDrainEvidenceStillRefusesAStartedEvent(t *testing.T) {
	observer, _, vm, job := observerFixture(t)
	vm.State, vm.CleanupRequested = "stopped", true
	if evidence, err := observer.DrainAndDeregister(context.Background(), vm, job); err == nil || evidence.Drained {
		t.Fatalf("a started event backed drain evidence: %+v %v", evidence, err)
	}
}

func TestACompletionInTheStartedBucketNeverRegistersTheRunner(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}); err != nil {
		t.Fatal(err)
	}
	misfiled := completedJob(job)
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Started: []github.Job{misfiled}}); err == nil {
		t.Fatal("a completion event was accepted as a started event")
	}
	vm, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if vm.State != "running" || vm.ExternalRunnerID != 0 || vm.ExternalRunnerName != "" {
		t.Fatalf("misfiled completion wrote registration identity: %+v", vm)
	}
}

func TestAStartedEventStillRegistersTheRunner(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}); err != nil {
		t.Fatal(err)
	}
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Started: []github.Job{job}}); err != nil {
		t.Fatal(err)
	}
	vm, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if vm.State != "registered" || vm.ExternalRunnerID != int64(job.RunnerID) || vm.ExternalRunnerName != job.RunnerName {
		t.Fatalf("started event did not register the runner: %+v", vm)
	}
}
