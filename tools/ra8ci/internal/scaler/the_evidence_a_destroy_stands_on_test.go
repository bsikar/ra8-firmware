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

// Stopping and destroying a runner VM is the one irreversible step in the
// lifecycle, so it runs on independent evidence that the runner is drained,
// idle, and the one this row owns. These hold what that evidence has to say.

// answeringObserver reports whatever registration and drain evidence a case
// wants, so the handler's judgement of it can be read on its own.
type answeringObserver struct {
	registered  RunnerObservation
	registerErr error
	drained     RunnerObservation
	drainErr    error
	drainCalls  int
}

func (o *answeringObserver) Registered(context.Context, store.RunnerVM, github.Job) (RunnerObservation, error) {
	return o.registered, o.registerErr
}

func (o *answeringObserver) DrainAndDeregister(_ context.Context, vm store.RunnerVM, _ github.Job) (RunnerObservation, error) {
	o.drainCalls++
	if o.drainErr != nil {
		return RunnerObservation{}, o.drainErr
	}
	evidence := o.drained
	if evidence.RunnerID == 0 {
		evidence.RunnerID = vm.ExternalRunnerID
	}
	if evidence.RunnerName == "" {
		evidence.RunnerName = vm.ExternalRunnerName
	}
	return evidence, nil
}

// soundDrain is the evidence a real drain produces: fresh, owned, idle.
func soundDrain(t *testing.T) RunnerObservation {
	t.Helper()
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	return RunnerObservation{EvidenceID: id, ObservedAt: time.Now(),
		Drained: true, NoActiveJob: true, RunnerDeregistered: true}
}

// A registration the runner API does not confirm, or confirms about some
// other runner, does not put the row into registered.
func TestAStartedEventNeedsRegistrationTheRunnerAPIConfirms(t *testing.T) {
	sound := soundDrain(t)

	for name, observation := range map[string]RunnerObservation{
		"another runner ID":      {RunnerID: 999, RunnerName: "runner-9000", EvidenceID: sound.EvidenceID, ObservedAt: time.Now()},
		"another runner name":    {RunnerID: 77, RunnerName: "runner-9999", EvidenceID: sound.EvidenceID, ObservedAt: time.Now()},
		"no evidence of its own": {RunnerID: 77, RunnerName: "runner-9000", ObservedAt: time.Now()},
		"evidence from an hour ago": {RunnerID: 77, RunnerName: "runner-9000",
			EvidenceID: sound.EvidenceID, ObservedAt: time.Now().Add(-time.Hour)},
	} {
		h, ledger, _, _, job := testHarness(t)
		vm := expired(t, h, ledger, job)
		h.runners = &answeringObserver{registered: observation}
		if err := h.started(context.Background(), job); err == nil {
			t.Fatalf("%s was accepted as registration", name)
		}
		after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
		if err != nil {
			t.Fatal(err)
		}
		if after.State != "running" || after.ExternalRunnerID != 0 {
			t.Fatalf("%s still moved the row: %+v", name, after)
		}
	}
}

// A runner API that cannot answer at all is reported rather than read as a
// runner that is not registered.
func TestAStartedEventReportsARunnerAPIThatCannotAnswer(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	h.runners = &answeringObserver{registerErr: errors.New("forge unreachable")}
	if err := h.started(context.Background(), job); err == nil {
		t.Fatal("an unreachable runner API was read as a clean refusal")
	}
	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.ExternalRunnerID != 0 {
		t.Fatalf("the row took a runner identity anyway: %+v", after)
	}
}

// Drain evidence that is stale, unowned, or does not say the runner is idle
// stops the destroy. Nothing here may reach the hypervisor.
func TestACompletedEventWillNotActOnDrainEvidenceItCannotTrust(t *testing.T) {
	sound := soundDrain(t)
	stale := sound
	stale.ObservedAt = time.Now().Add(-time.Hour)
	unmeasured := sound
	unmeasured.EvidenceID = "not-an-id"
	busy := sound
	busy.NoActiveJob = false
	wet := sound
	wet.Drained = false
	elsewhere := sound
	elsewhere.RunnerID = 4242
	misnamed := sound
	misnamed.RunnerName = "runner-4242"

	for name, evidence := range map[string]RunnerObservation{
		"evidence from an hour ago": stale,
		"evidence with no ID":       unmeasured,
		"a runner still on a job":   busy,
		"a runner not drained":      wet,
		"another runner's ID":       elsewhere,
		"another runner's name":     misnamed,
	} {
		h, ledger, fake, _, job := testHarness(t)
		vm := expired(t, h, ledger, job)
		h.runners = &answeringObserver{drained: evidence}
		if err := h.completed(context.Background(), completedJob(job)); err == nil {
			t.Fatalf("%s was accepted as drain evidence", name)
		}
		after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
		if err != nil {
			t.Fatal(err)
		}
		if after.State == "released" {
			t.Fatalf("%s still released the row", name)
		}
		if fake.deleteCalls != 0 {
			t.Fatalf("%s still destroyed the guest", name)
		}
	}
}

// A drain the runner API cannot perform is reported, and the row is left
// where a later pass can pick it up rather than being called released.
func TestACompletedEventReportsADrainThatCouldNotBeDone(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	observer := &answeringObserver{drainErr: errors.New("forge unreachable")}
	h.runners = observer
	if err := h.completed(context.Background(), completedJob(job)); err == nil {
		t.Fatal("a drain that could not be done was read as a completed drain")
	}
	if observer.drainCalls == 0 {
		t.Fatal("the drain was never attempted")
	}
	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State == "released" || fake.deleteCalls != 0 {
		t.Fatalf("a failed drain still finished the row: %+v deletes=%d", after, fake.deleteCalls)
	}
}

// Destroy needs the deregistration on top of the drain: a runner still
// registered with the forge would come back as a ghost.
func TestADestroyNeedsTheRunnerDeregisteredAsWell(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	stillRegistered := soundDrain(t)
	stillRegistered.RunnerDeregistered = false
	h.runners = &answeringObserver{drained: stillRegistered}
	if err := h.completed(context.Background(), completedJob(job)); err == nil {
		t.Fatal("a still-registered runner was destroyed")
	}
	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if fake.deleteCalls != 0 {
		t.Fatalf("the guest was destroyed anyway: %+v", after)
	}
}
