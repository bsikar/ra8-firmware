// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A scale-set event carries the same identity fields whichever transition it
// reports, so nearly every refusal in the started and completed steps is a
// disagreement between the event and the durable row. These hold that set.

func TestAStartedStepRefusesAnEventItCannotStandOn(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	ctx := context.Background()

	wrongRun := job
	wrongRun.WorkflowRunID = job.WorkflowRunID + 1
	wrongRequest := job
	wrongRequest.RunnerRequestID = job.RunnerRequestID + 1
	wrongRepository := job
	wrongRepository.Repository = "another-repository"
	noRunnerID := job
	noRunnerID.RunnerID = 0
	noRunnerName := job
	noRunnerName.RunnerName = ""

	for name, event := range map[string]github.Job{
		"a completed event":               completedJob(job),
		"an assigned event":               assignedJob(job),
		"another workflow run":            wrongRun,
		"another runner request":          wrongRequest,
		"another repository":              wrongRepository,
		"a started job with no runner ID": noRunnerID,
		"a started job with no runner":    noRunnerName,
	} {
		if err := h.started(ctx, event); err == nil {
			t.Fatalf("%s was accepted as a started event", name)
		}
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "running" || after.ExternalRunnerID != 0 {
		t.Fatalf("a refused started event still moved the row: %+v", after)
	}
}

// Each refusal names its own reason: an operator reading the log has to be
// able to tell a wrong-kind event from a runner the row does not own.
func TestAStartedRefusalNamesWhatDisagreed(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	ctx := context.Background()
	if err := h.started(ctx, completedJob(job)); err == nil ||
		!strings.Contains(err.Error(), "not a job-started message") {
		t.Fatalf("a completed event = %v", err)
	}
	wrongRun := job
	wrongRun.WorkflowRunID = job.WorkflowRunID + 1
	if err := h.started(ctx, wrongRun); err == nil ||
		!strings.Contains(err.Error(), "does not match durable GitHub job") {
		t.Fatalf("another workflow run = %v", err)
	}
	noRunner := job
	noRunner.RunnerID = 0
	if err := h.started(ctx, noRunner); err == nil ||
		!strings.Contains(err.Error(), "lacks a running owned runner") {
		t.Fatalf("a started job with no runner ID = %v", err)
	}
}

// A job whose reservation the ledger has never heard of is a read failure,
// not a quiet success: unlike the completed step, there is nothing here to
// be idempotent about.
func TestAStartedEventForAnUnknownJobIsRefused(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	stranger := job
	stranger.JobID = "job-nobody-reserved"
	err := h.started(context.Background(), stranger)
	if !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("an unknown job = %v, want the ledger's not-found", err)
	}
}

// Registration is idempotent, and the second pass is where a runner swapped
// underneath the row would show: the same message is a no-op, a different
// runner on the same job is refused.
func TestASecondStartedEventMustAgreeWithTheRunnerOnFile(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	ctx := context.Background()
	if err := h.started(ctx, job); err != nil {
		t.Fatalf("first started event: %v", err)
	}
	registered, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if registered.State != "registered" || registered.ExternalRunnerID != int64(job.RunnerID) {
		t.Fatalf("runner not registered: %+v", registered)
	}
	if err := h.started(ctx, job); err != nil {
		t.Fatalf("a repeated started event: %v", err)
	}
	swapped := job
	swapped.RunnerName = "runner-9099"
	if err := h.started(ctx, swapped); err == nil ||
		!strings.Contains(err.Error(), "disagrees with durable runner identity") {
		t.Fatalf("a swapped runner name = %v", err)
	}
	renumbered := job
	renumbered.RunnerID = job.RunnerID + 1
	if err := h.started(ctx, renumbered); err == nil ||
		!strings.Contains(err.Error(), "disagrees with durable runner identity") {
		t.Fatalf("a swapped runner ID = %v", err)
	}
}

// The completed step is the one that tears a guest down, so an event that is
// not GitHub's terminal statement, or is missing the parts that make it
// terminal, is refused before the row is read.
func TestACompletedStepRefusesAnEventThatIsNotTerminal(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	ctx := context.Background()

	noResult := completedJob(job)
	noResult.Result = ""
	noFinish := completedJob(job)
	noFinish.FinishTime = time.Time{}
	fromTheFuture := completedJob(job)
	fromTheFuture.FinishTime = time.Now().UTC().Add(time.Hour)

	for name, event := range map[string]github.Job{
		"a started event":             job,
		"an assigned event":           assignedJob(job),
		"no result":                   noResult,
		"no finish time":              noFinish,
		"a finish time in the future": fromTheFuture,
	} {
		err := h.completed(ctx, event)
		if err == nil || !strings.Contains(err.Error(), "lacks terminal result or finish time") {
			t.Fatalf("%s = %v", name, err)
		}
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a refused completed event still touched the hypervisor: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// A terminal event for a job no VM was ever reserved for is a success with
// nothing to do: the scale set is entitled to report jobs this handler never
// took, and failing them would stall the queue behind them.
func TestACompletedEventForAJobWithNoReservationIsAccepted(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)
	stranger := completedJob(job)
	stranger.JobID = "job-nobody-reserved"
	if err := h.completed(context.Background(), stranger); err != nil {
		t.Fatalf("a completed event with no reservation = %v", err)
	}
}

// But a terminal event that disagrees with the row it found is refused, and
// the reservation is left exactly as it was.
func TestACompletedEventMustMatchTheRowItFound(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	ctx := context.Background()
	mismatched := completedJob(job)
	mismatched.WorkflowRunID = job.WorkflowRunID + 1
	err := h.completed(ctx, mismatched)
	if err == nil || !strings.Contains(err.Error(), "does not match durable GitHub job") {
		t.Fatalf("another workflow run = %v", err)
	}
	after, err := ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "running" || after.CleanupRequested {
		t.Fatalf("a refused completed event moved the row: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a refused completed event touched the hypervisor: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}
