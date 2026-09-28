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

// Drain evidence is what the ledger is shown before a guest is stopped or
// deleted, so it is held to the runner this reservation owns, to a clock,
// and for a destroy to a reviewed approval and a guest observed safe. These
// hold the refusals; the happy stop and destroy are already held end to end
// by handler_test.go.

// scriptedObserver answers the drain seam with whatever the test holds,
// leaving registration to the ordinary fake.
type scriptedObserver struct {
	answer  RunnerObservation
	failure error
	asked   int
}

func (o *scriptedObserver) Registered(_ context.Context, _ store.RunnerVM, job github.Job) (RunnerObservation, error) {
	id, _ := store.NewID()
	return RunnerObservation{RunnerID: int64(job.RunnerID), RunnerName: job.RunnerName, EvidenceID: id, ObservedAt: time.Now()}, nil
}

func (o *scriptedObserver) DrainAndDeregister(_ context.Context, _ store.RunnerVM, _ github.Job) (RunnerObservation, error) {
	o.asked++
	if o.failure != nil {
		return RunnerObservation{}, o.failure
	}
	return o.answer, nil
}

// registeredReservation runs a reservation up to a registered runner, the
// state every drain starts from.
func registeredReservation(t *testing.T, h *Handler, ledger *memoryLedger, job github.Job) store.RunnerVM {
	t.Helper()
	vm := expired(t, h, ledger, job)
	if err := h.started(context.Background(), job); err != nil {
		t.Fatalf("register the runner: %v", err)
	}
	registered, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if registered.State != "registered" || registered.ExternalRunnerID != int64(job.RunnerID) {
		t.Fatalf("fixture is not a registered reservation: %+v", registered)
	}
	return registered
}

func TestDrainEvidenceIsHeldToTheRunnerThisReservationOwns(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	registered := registeredReservation(t, h, ledger, job)
	ctx := context.Background()
	drained := RunnerObservation{RunnerID: registered.ExternalRunnerID, RunnerName: registered.ExternalRunnerName,
		ObservedAt: time.Now(), Drained: true, NoActiveJob: true, RunnerDeregistered: true}
	valid, _ := store.NewID()

	stale := drained
	stale.EvidenceID = valid
	stale.ObservedAt = time.Now().Add(-time.Hour)
	unusable := drained
	unusable.EvidenceID = "not-an-id"
	anotherRunner := drained
	anotherRunner.EvidenceID = valid
	anotherRunner.RunnerID = registered.ExternalRunnerID + 1
	renamed := drained
	renamed.EvidenceID = valid
	renamed.RunnerName = "runner-9099"
	stillWorking := drained
	stillWorking.EvidenceID = valid
	stillWorking.NoActiveJob = false
	notDrained := drained
	notDrained.EvidenceID = valid
	notDrained.Drained = false

	for name, answer := range map[string]RunnerObservation{
		"an observation from an hour ago": stale,
		"unusable evidence":               unusable,
		"another runner's ID":             anotherRunner,
		"another runner's name":           renamed,
		"a runner still holding a job":    stillWorking,
		"a runner that never drained":     notDrained,
	} {
		observer := &scriptedObserver{answer: answer}
		h.runners = observer
		_, err := h.drainEvidence(ctx, registered, job, false)
		if err == nil || !strings.Contains(err.Error(), "lacks fresh exact ownership and idle evidence") {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A reservation with no registered runner has nothing to drain, so the
// evidence is refused on the row rather than on the observation.
func TestDrainEvidenceRefusesAReservationWithNoRunnerOnFile(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	_, err := h.drainEvidence(context.Background(), vm, job, false)
	if err == nil || !strings.Contains(err.Error(), "lacks fresh exact ownership and idle evidence") {
		t.Fatalf("a reservation with no runner = %v", err)
	}
}

// An observer that cannot reach the forge fails the step with its own
// reason: nothing is assumed about the runner in the meantime.
func TestDrainEvidenceCarriesTheObserversOwnFailure(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	registered := registeredReservation(t, h, ledger, job)
	forgeDown := errors.New("runner administration API is unavailable")
	observer := &scriptedObserver{failure: forgeDown}
	h.runners = observer
	_, err := h.drainEvidence(context.Background(), registered, job, false)
	if !errors.Is(err, forgeDown) {
		t.Fatalf("an unreachable forge = %v", err)
	}
	if observer.asked != 1 {
		t.Fatalf("the forge was asked %d times", observer.asked)
	}
}

// A stop only needs the runner idle. The proof it produces carries no
// approval and no digest, because nothing is being deleted.
func TestStopEvidenceCarriesNoCleanupApproval(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	registered := registeredReservation(t, h, ledger, job)
	proof, err := h.drainEvidence(context.Background(), registered, job, false)
	if err != nil {
		t.Fatalf("drain evidence for a stop: %v", err)
	}
	if !proof.Drained || !proof.NoActiveJob || proof.ExternalRunnerID != registered.ExternalRunnerID {
		t.Fatalf("stop proof = %+v", proof)
	}
	if proof.ApprovalID != "" || proof.ExpectedConfigDigest != "" {
		t.Fatalf("a stop carried cleanup authority it does not need: %+v", proof)
	}
}

// A destroy needs two things a stop does not: the runner actually gone from
// the forge, and an operator's reviewed cleanup approval.
func TestDestroyEvidenceNeedsDeregistrationAndAReviewedApproval(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	registered := registeredReservation(t, h, ledger, job)
	ctx := context.Background()
	valid, _ := store.NewID()
	stillRegistered := RunnerObservation{RunnerID: registered.ExternalRunnerID, RunnerName: registered.ExternalRunnerName,
		EvidenceID: valid, ObservedAt: time.Now(), Drained: true, NoActiveJob: true, RunnerDeregistered: false}

	h.runners = &scriptedObserver{answer: stillRegistered}
	if _, err := h.drainEvidence(ctx, registered, job, true); err == nil ||
		!strings.Contains(err.Error(), "reviewed deregistration and cleanup approval required") {
		t.Fatalf("a runner still registered = %v", err)
	}

	h.runners = testObserver{}
	h.config.CleanupApprovalID = ""
	if _, err := h.drainEvidence(ctx, registered, job, true); err == nil ||
		!strings.Contains(err.Error(), "reviewed deregistration and cleanup approval required") {
		t.Fatalf("no cleanup approval = %v", err)
	}
}

// And the guest itself is observed one more time: a destroy is authorized
// against the digest read here, not the one the reservation was written
// with, so a guest that is not stopped stops the step.
func TestDestroyEvidenceRefusesAGuestThatIsNotStopped(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	registered := registeredReservation(t, h, ledger, job)
	_, err := h.drainEvidence(context.Background(), registered, job, true)
	if err == nil || !strings.Contains(err.Error(), "not safe for reviewed cleanup") {
		t.Fatalf("a running guest = %v", err)
	}
}

// permissiveLedger opens an operation for any kind, which is the only way
// to reach the switch's own refusal: the real ledger rejects an unknown
// kind before the hypervisor is ever chosen.
type permissiveLedger struct {
	*memoryLedger
	opened []string
}

func (l *permissiveLedger) BeginRunnerVMOperation(_ context.Context, _, id string, generation int64, kind string, _ store.RunnerVMSafetyEvidence) (store.RunnerVMOperation, error) {
	l.opened = append(l.opened, kind)
	operationID, err := store.NewID()
	if err != nil {
		return store.RunnerVMOperation{}, err
	}
	return store.RunnerVMOperation{ID: operationID, RunnerVMID: id, Generation: generation, Kind: kind}, nil
}

func TestAnUnsupportedOperationIsRefusedAfterItIsRecorded(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	permissive := &permissiveLedger{memoryLedger: ledger}
	h.ledger = permissive
	fake.mu.Lock()
	startsBefore, stopsBefore, deletesBefore := fake.startCalls, fake.stopCalls, fake.deleteCalls
	fake.mu.Unlock()
	_, err := h.execute(context.Background(), vm, "reboot", store.RunnerVMSafetyEvidence{})
	if err == nil || !strings.Contains(err.Error(), "unsupported VM operation") {
		t.Fatalf("an unsupported operation = %v", err)
	}
	if len(permissive.opened) != 1 || permissive.opened[0] != "reboot" {
		t.Fatalf("operations opened: %v", permissive.opened)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.startCalls != startsBefore || fake.stopCalls != stopsBefore || fake.deleteCalls != deletesBefore {
		t.Fatalf("an unsupported operation reached the hypervisor: start=%d stop=%d delete=%d",
			fake.startCalls, fake.stopCalls, fake.deleteCalls)
	}
}
