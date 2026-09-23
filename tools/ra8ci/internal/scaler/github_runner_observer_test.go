// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type observerAdminFake struct {
	identity github.RunnerIdentity
	exists   bool
	err      error
	calls    int
}

func (a *observerAdminFake) RunnerByID(_ context.Context, id int) (github.RunnerIdentity, bool, error) {
	a.calls++
	if a.err != nil {
		return github.RunnerIdentity{}, false, a.err
	}
	if a.identity.ID != id {
		return github.RunnerIdentity{}, false, nil
	}
	return a.identity, a.exists, nil
}

func observerFixture(t *testing.T) (*GitHubRunnerObserver, *observerAdminFake, store.RunnerVM, github.Job) {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	admin := &observerAdminFake{identity: github.RunnerIdentity{ID: 77, Name: "runner-9000"}, exists: true}
	observer, err := NewGitHubRunnerObserver(42, admin)
	if err != nil {
		t.Fatal(err)
	}
	vm := store.RunnerVM{ID: id, RunnerVMInput: store.RunnerVMInput{
		ScaleSetID: 42, JobID: "job-1", RunnerRequestID: 4, WorkflowRunID: 12,
		Repository: "bsikar/ra8-firmware", WorkflowRef: "refs/heads/test", VMID: 9000, Name: "ra8-lab-ci-9000",
	}, State: "running", Generation: 1}
	job := github.Job{JobID: "job-1", RunnerRequestID: 4, WorkflowRunID: 12, Repository: "bsikar/ra8-firmware", WorkflowRef: "refs/heads/test", RunnerID: 77, RunnerName: "runner-9000"}
	return observer, admin, vm, job
}

func TestGitHubRunnerObserverVerifiesExactRegistration(t *testing.T) {
	observer, admin, vm, job := observerFixture(t)
	evidence, err := observer.Registered(context.Background(), vm, job)
	if err != nil {
		t.Fatal(err)
	}
	if evidence.RunnerID != 77 || evidence.RunnerName != "runner-9000" ||
		!store.ValidID(evidence.EvidenceID) || evidence.ObservedAt.IsZero() ||
		evidence.ObservedAt.After(time.Now().Add(time.Second)) ||
		evidence.Drained || evidence.NoActiveJob || evidence.RunnerDeregistered {
		t.Fatalf("invalid or overclaimed registration evidence: %+v", evidence)
	}
	if admin.calls != 1 {
		t.Fatalf("GitHub API calls=%d, want 1", admin.calls)
	}
}

func TestGitHubRunnerObserverRejectsMissingForeignOrUnboundRunner(t *testing.T) {
	tests := []struct {
		name   string
		change func(*observerAdminFake, *store.RunnerVM, *github.Job)
	}{
		{"missing", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) { a.exists = false }},
		{"foreign name", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) { a.identity.Name = "runner-9001" }},
		{"foreign id", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) { a.identity.ID = 78 }},
		{"unbound job name", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) { j.RunnerName = "runner-9001" }},
		{"wrong scale set", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.ScaleSetID = 43 }},
		{"wrong VM state", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.State = "stopped" }},
		{"unknown VM outcome", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.UnknownOutcome = true }},
		{"cleanup requested", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.CleanupRequested = true }},
		{"wrong durable job", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) { j.JobID = "other" }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			o, _, vm, job := observerFixture(t)
			tt.change(o.admin.(*observerAdminFake), &vm, &job)
			if evidence, err := o.Registered(context.Background(), vm, job); err == nil || evidence.EvidenceID != "" {
				t.Fatalf("unsafe registration accepted: %+v %v", evidence, err)
			}
		})
	}
}

func TestGitHubRunnerObserverFailsClosedOnDrain(t *testing.T) {
	o, admin, vm, _ := observerFixture(t)
	evidence, err := o.DrainAndDeregister(context.Background(), vm)
	if !errors.Is(err, ErrGuestDrainProtocolUnavailable) {
		t.Fatalf("error=%v", err)
	}
	if evidence.Drained || evidence.NoActiveJob || evidence.RunnerDeregistered || evidence.EvidenceID != "" || admin.calls != 0 {
		t.Fatalf("drain synthesized proof or queried GitHub: %+v calls=%d", evidence, admin.calls)
	}
	admin.err = errors.New("GitHub API unavailable")
	_, err = o.Registered(context.Background(), vm, github.Job{JobID: "job-1", RunnerRequestID: 4, WorkflowRunID: 12, Repository: "bsikar/ra8-firmware", WorkflowRef: "refs/heads/test", RunnerID: 77, RunnerName: "runner-9000"})
	if err == nil {
		t.Fatal("GitHub API failure was accepted")
	}
}
