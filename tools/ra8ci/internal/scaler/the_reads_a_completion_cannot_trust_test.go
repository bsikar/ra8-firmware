// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A completion reads the reservation before it decides anything. When that
// read, or the drain that follows it, cannot be trusted, the completion has to
// stop rather than guess: an unreachable ledger is not an absent reservation,
// and a drain that did not land is not a guest that may be deleted.

// unreliableLedger wraps the in-memory ledger and fails one named call. It
// wraps rather than forks so every other call answers exactly as the harness's
// own ledger does.
type unreliableLedger struct {
	Ledger
	byJobErr error
	drainErr error
	drained  int
}

func (u *unreliableLedger) GetRunnerVMByJob(ctx context.Context, scaleSet int64, jobID string) (store.RunnerVM, error) {
	if u.byJobErr != nil {
		return store.RunnerVM{}, u.byJobErr
	}
	return u.Ledger.GetRunnerVMByJob(ctx, scaleSet, jobID)
}

func (u *unreliableLedger) MarkRunnerVMDraining(ctx context.Context, actor, id string, generation int64) (store.RunnerVM, error) {
	u.drained++
	if u.drainErr != nil {
		return store.RunnerVM{}, u.drainErr
	}
	return u.Ledger.MarkRunnerVMDraining(ctx, actor, id, generation)
}

// An unreachable ledger is handed back as itself. It must not be read as
// store.ErrNotFound, which is the one answer that lets a completion pass.
func TestACompletionCarriesAnUnreachableLedgerBackRatherThanPassing(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	unreachable := errors.New("ledger unreachable")
	broken := &unreliableLedger{Ledger: ledger, byJobErr: unreachable}
	h.ledger = broken

	err := h.completed(context.Background(), completedJob(job))
	if !errors.Is(err, unreachable) {
		t.Fatalf("error = %v, want the ledger failure carried back", err)
	}
	if broken.drained != 0 {
		t.Fatalf("a completion drained a reservation it never read: %d", broken.drained)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a completion tore down a guest it never read: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// store.ErrNotFound is the one read failure that is not a failure: no VM was
// ever reserved for the job, so the completion has nothing to do.
func TestACompletionPassesWhenNoReservationWasEverTaken(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	expired(t, h, ledger, job)
	h.ledger = &unreliableLedger{Ledger: ledger, byJobErr: store.ErrNotFound}

	if err := h.completed(context.Background(), completedJob(job)); err != nil {
		t.Fatalf("a completion over an unreserved job was refused: %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("an unreserved job moved a guest: stop=%d delete=%d", fake.stopCalls, fake.deleteCalls)
	}
}

// A drain that did not land stops the completion where it stands. Reading a
// failed drain as a drained row is how a guest still running a job gets torn
// down under it.
func TestACompletionStopsWhenTheDrainDoesNotLand(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	refused := errors.New("draining write refused")
	broken := &unreliableLedger{Ledger: ledger, drainErr: refused}
	h.ledger = broken

	err := h.completed(context.Background(), completedJob(job))
	if !errors.Is(err, refused) {
		t.Fatalf("error = %v, want the refused drain carried back", err)
	}
	if broken.drained != 1 {
		t.Fatalf("the drain was attempted %d times, want once", broken.drained)
	}

	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.CleanupRequested || after.State == "draining" {
		t.Fatalf("a refused drain moved the row anyway: %+v", after)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("a guest was torn down after a refused drain: stop=%d delete=%d",
			fake.stopCalls, fake.deleteCalls)
	}
}

// The three answers a completion can give to a bad read are distinguishable:
// an unreachable ledger and a refused drain each carry their own cause, and
// neither reads as the message-mismatch refusal.
func TestTheReadFailuresACompletionReportsAreEachTheirOwn(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	expired(t, h, ledger, job)

	h.ledger = &unreliableLedger{Ledger: ledger, byJobErr: errors.New("ledger unreachable")}
	unreadable := h.completed(context.Background(), completedJob(job))
	h.ledger = &unreliableLedger{Ledger: ledger, drainErr: errors.New("draining write refused")}
	undrained := h.completed(context.Background(), completedJob(job))

	if unreadable == nil || undrained == nil || unreadable.Error() == undrained.Error() {
		t.Fatalf("unreadable = %v, undrained = %v, want two distinct causes", unreadable, undrained)
	}
	for _, err := range []error{unreadable, undrained} {
		if strings.Contains(err.Error(), "does not match durable GitHub job") {
			t.Fatalf("a read failure was reported as a message mismatch: %v", err)
		}
	}
}
