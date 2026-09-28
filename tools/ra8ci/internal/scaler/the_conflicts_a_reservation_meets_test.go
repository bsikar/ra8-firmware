// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// quarrelsomeLedger answers the first lookup honestly and the second one
// badly, which is the shape of a ledger that loses its connection partway
// through a reservation: the row was not found, the reserve conflicted, and
// then the read that would explain the conflict cannot be made.
type quarrelsomeLedger struct {
	Ledger
	lookups int
	lost    error
}

func (l *quarrelsomeLedger) GetRunnerVMByJob(ctx context.Context, scaleSetID int64, jobID string) (store.RunnerVM, error) {
	l.lookups++
	if l.lookups == 1 {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return store.RunnerVM{}, l.lost
}

func (l *quarrelsomeLedger) ReserveRunnerVM(context.Context, string, store.RunnerVMInput, time.Time) (store.RunnerVM, bool, error) {
	return store.RunnerVM{}, false, store.ErrConflict
}

// A conflict means somebody else reserved this job first, and the lookup is
// how this plane finds out whether that reservation is the same job. When
// the lookup itself fails, the conflict stays unexplained and the failure is
// handed back rather than read as "no VMID free", which would send an
// operator hunting for capacity that is not the problem.
func TestAnUnexplainableConflictIsHandedBackNotReadAsExhaustion(t *testing.T) {
	h, _, fake, _, job := testHarness(t)
	lost := errors.New("ledger connection lost")
	h.ledger = &quarrelsomeLedger{Ledger: h.ledger, lost: lost}

	_, err := h.reservation(context.Background(), assignedJob(job))
	if !errors.Is(err, lost) {
		t.Fatalf("error = %v, want the lookup failure itself", err)
	}
	if err != nil && strings.Contains(err.Error(), "no approved disposable VMID is free") {
		t.Fatal("an unexplained conflict was reported as pool exhaustion")
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 {
		t.Fatalf("a guest was cloned for a reservation that never opened: %d", fake.cloneCalls)
	}
}

// The lease bounds are checked when the deadline is minted, not only when a
// caller supplies one, so a deployment configured with a lease the store
// will not accept is refused before a guest exists rather than after.
func TestAReservationRefusesALeaseTheStoreWillNotAccept(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	h.config.UnclaimedLease = time.Second

	_, err := h.reservation(context.Background(), assignedJob(job))
	if err == nil || !strings.Contains(err.Error(), "unclaimed lease") {
		t.Fatalf("error = %v, want a refusal naming the lease bounds", err)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("error = %v, want it to carry store.ErrInvalid", err)
	}
	if ledger.reserveCalls != 0 {
		t.Fatalf("a reservation was opened with an unacceptable lease: %d", ledger.reserveCalls)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 {
		t.Fatalf("a guest was cloned for a lease the store refuses: %d", fake.cloneCalls)
	}
}

// The VMID allowlist is the disposable pool, so a duplicate in it is a
// configuration mistake rather than a harmless repeat: the pool would look
// larger than it is and two jobs could be pointed at one guest.
func TestADuplicateVMIDIsRefusedAtConstruction(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	cfg := h.config
	cfg.VMIDs = []int{9000, 9002, 9000}
	_, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission)
	if err == nil || !strings.Contains(err.Error(), "duplicate") {
		t.Fatalf("error = %v, want a refusal naming the duplicate", err)
	}
	cfg.VMIDs = []int{9000, 9002}
	if _, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err != nil {
		t.Fatalf("a distinct allowlist was refused: %v", err)
	}
}

// An unwired handler reaps nothing rather than reporting an empty pass. A
// zero report from a handler that never asked the ledger anything reads to
// an operator exactly like a queue with nothing in it.
func TestReapingNeedsAWiredHandler(t *testing.T) {
	report, err := (&Handler{}).ReapUnclaimed(context.Background(), &recordingRevoker{})
	if err == nil || !strings.Contains(err.Error(), "needs a wired handler") {
		t.Fatalf("error = %v, want a refusal naming the unwired handler", err)
	}
	if !reflect.DeepEqual(report, UnclaimedReport{}) {
		t.Fatalf("report = %+v, want nothing claimed", report)
	}
	if _, err := (&Handler{}).ExpiredUnclaimed(context.Background(), time.Now()); err == nil {
		t.Fatal("an unwired handler showed a queue")
	}
	h, _, _, _, _ := testHarness(t)
	if _, err := h.ExpiredUnclaimed(context.Background(), time.Time{}); err == nil ||
		!strings.Contains(err.Error(), "needs a clock") {
		t.Fatalf("error = %v, want a refusal naming the missing clock", err)
	}
}
