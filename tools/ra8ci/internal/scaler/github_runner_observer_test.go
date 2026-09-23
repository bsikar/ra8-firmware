// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/actions/scaleset"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type observerAdminFake struct {
	identity          github.RunnerIdentity
	exists            bool
	err               error
	calls             int
	removeCalls       int
	removeErr         error
	remainAfterRemove bool
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

func (a *observerAdminFake) RemoveRunner(_ context.Context, id int) error {
	a.removeCalls++
	if a.removeErr != nil {
		return a.removeErr
	}
	if a.identity.ID != id || !a.exists {
		return errors.New("runner missing or mismatched")
	}
	if !a.remainAfterRemove {
		a.exists = false
	}
	return nil
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
	}, State: "running", Generation: 1, ExternalRunnerID: 77, ExternalRunnerName: "runner-9000"}
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

func TestGitHubRunnerObserverDeregistersOnlyCompletedExactRunner(t *testing.T) {
	o, admin, vm, job := observerFixture(t)
	vm.State = "draining"
	vm.CleanupRequested = true
	job.Kind = scaleset.MessageTypeJobCompleted
	job.Result = "Succeeded"
	job.FinishTime = time.Now().UTC()
	evidence, err := o.DrainAndDeregister(context.Background(), vm, job)
	if err != nil {
		t.Fatal(err)
	}
	if !evidence.Drained || !evidence.NoActiveJob || !evidence.RunnerDeregistered || evidence.RunnerID != 77 || evidence.RunnerName != "runner-9000" || !store.ValidID(evidence.EvidenceID) {
		t.Fatalf("invalid drain evidence: %+v", evidence)
	}
	if admin.removeCalls != 1 || admin.exists || admin.calls != 2 {
		t.Fatalf("remove=%d exists=%v lookups=%d", admin.removeCalls, admin.exists, admin.calls)
	}
}

func TestGitHubRunnerObserverDrainIsIdempotentWhenAlreadyAbsent(t *testing.T) {
	o, admin, vm, job := observerFixture(t)
	vm.State = "stopped"
	vm.CleanupRequested = true
	admin.exists = false
	job.Kind = scaleset.MessageTypeJobCompleted
	job.Result = "Cancelled"
	job.FinishTime = time.Now().UTC()
	evidence, err := o.DrainAndDeregister(context.Background(), vm, job)
	if err != nil {
		t.Fatal(err)
	}
	if !evidence.RunnerDeregistered || admin.removeCalls != 0 || admin.calls != 2 {
		t.Fatalf("evidence=%+v remove=%d lookups=%d", evidence, admin.removeCalls, admin.calls)
	}
}

func TestGitHubRunnerObserverDrainFailsClosedForInvalidEvidenceOrAdminFailure(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(*observerAdminFake, *store.RunnerVM, *github.Job)
	}{
		{"no terminal event", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) {
			j.Kind = scaleset.MessageTypeJobAvailable
		}},
		{"missing result", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) { j.Result = "" }},
		{"future completion", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) { j.FinishTime = time.Now().Add(time.Hour) }},
		{"wrong runner", func(_ *observerAdminFake, _ *store.RunnerVM, j *github.Job) { j.RunnerID++ }},
		{"wrong reservation", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.ExternalRunnerID++ }},
		{"wrong state", func(_ *observerAdminFake, v *store.RunnerVM, _ *github.Job) { v.State = "running" }},
		{"foreign registration", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) { a.identity.Name = "runner-9001" }},
		{"remove error", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) {
			a.removeErr = errors.New("API unavailable")
		}},
		{"still registered", func(a *observerAdminFake, _ *store.RunnerVM, _ *github.Job) { a.remainAfterRemove = true }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			o, admin, vm, job := observerFixture(t)
			vm.State = "draining"
			vm.CleanupRequested = true
			job.Kind = scaleset.MessageTypeJobCompleted
			job.Result = "Succeeded"
			job.FinishTime = time.Now().UTC()
			tt.mutate(admin, &vm, &job)
			evidence, err := o.DrainAndDeregister(context.Background(), vm, job)
			if err == nil || evidence.EvidenceID != "" {
				t.Fatalf("unsafe drain accepted: %+v %v", evidence, err)
			}
		})
	}
}

func TestGitHubRunnerObserverRegistrationFailsClosedOnAPIError(t *testing.T) {
	o, admin, vm, job := observerFixture(t)
	admin.err = errors.New("GitHub API unavailable")
	if _, err := o.Registered(context.Background(), vm, job); err == nil {
		t.Fatal("GitHub API failure was accepted")
	}
}
