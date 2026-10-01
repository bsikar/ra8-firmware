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

// The assignment step is the only one that launches anything, so what it
// refuses to launch on is the interesting half. These hold the gate in front
// of a launch, the guest observation before a start, and the receipt a JIT
// bootstrap has to come back with.

func TestAnAssignmentRefusesAnEventOfAnotherKind(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	ctx := context.Background()
	for name, event := range map[string]github.Job{
		"a started event":   job,
		"a completed event": completedJob(job),
	} {
		if err := h.assigned(ctx, event); err == nil ||
			!strings.Contains(err.Error(), "not a job-assigned message") {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if _, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("a refused assignment still reserved a VM: %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 {
		t.Fatalf("a refused assignment cloned %d guest(s)", fake.cloneCalls)
	}
}

// Nothing is launched while the off-VM backup gate is unhappy: a guest
// cloned now could not be restored from if the lab were lost, so the gate
// sits in front of the clone rather than after it.
func TestNoGuestIsLaunchedWhileTheBackupGateRefuses(t *testing.T) {
	h, ledger, fake, bootstrap, job := testHarness(t)
	h.backup = testBackupGate{err: errors.New("no full backup inside the window")}
	ctx := context.Background()
	err := h.assigned(ctx, assignedJob(job))
	if err == nil || !strings.Contains(err.Error(), "off-VM backup gate") {
		t.Fatalf("an assignment under a refusing gate = %v", err)
	}
	if !strings.Contains(err.Error(), "no full backup inside the window") {
		t.Fatalf("the gate's own reason was dropped: %v", err)
	}
	// The gate is judged before the reservation is written, so a refused
	// assignment leaves no row behind at all: the next pass starts from a
	// clean queue rather than from a half-made reservation.
	if _, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("a refused assignment left a reservation behind: %v", err)
	}
	if bootstrap.calls != 0 {
		t.Fatalf("bootstrap ran %d time(s) under a refusing gate", bootstrap.calls)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 || fake.startCalls != 0 {
		t.Fatalf("hypervisor touched under a refusing gate: clone=%d start=%d", fake.cloneCalls, fake.startCalls)
	}
}

// The guest is observed again immediately before the start, because the
// ledger's idea of stopped is older than the hypervisor's.
func TestAStartNeedsAGuestThatIsStableAndStopped(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	before := func() int {
		fake.mu.Lock()
		defer fake.mu.Unlock()
		return fake.startCalls
	}()
	err := h.prepareAndStart(context.Background(), vm)
	if err == nil || !strings.Contains(err.Error(), "not stable and stopped before start") {
		t.Fatalf("a running guest = %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.startCalls != before {
		t.Fatalf("a refused start still asked the hypervisor: %d -> %d", before, fake.startCalls)
	}
}

// The JIT bootstrap hands a runner credential to a guest, so the reservation
// it runs against has to be running, reconciled and not on its way out.
func TestABootstrapNeedsAReconciledRunningReservation(t *testing.T) {
	h, ledger, _, bootstrap, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	before := bootstrap.calls

	stopped := vm
	stopped.State = "stopped"
	unreconciled := vm
	unreconciled.UnknownOutcome = true
	leaving := vm
	leaving.CleanupRequested = true

	for name, candidate := range map[string]store.RunnerVM{
		"a stopped reservation":     stopped,
		"an unreconciled one":       unreconciled,
		"one already being cleaned": leaving,
		"a zero-value reservation":  {},
	} {
		if err := h.bootstrapRunning(context.Background(), candidate); err == nil ||
			!strings.Contains(err.Error(), "requires a reconciled running reservation") {
			t.Fatalf("%s = %v", name, err)
		}
	}
	if bootstrap.calls != before {
		t.Fatalf("bootstrap was asked %d extra time(s) for a reservation it should refuse", bootstrap.calls-before)
	}
}

// A bootstrapper that cannot reach the JIT channel fails the step with its
// own reason carried through rather than a generic one.
func TestABootstrapThatCannotRunIsReportedWithItsReason(t *testing.T) {
	h, ledger, _, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	h.bootstrap = failedBootstrap{}
	err := h.bootstrapRunning(context.Background(), vm)
	if err == nil || !strings.Contains(err.Error(), "post-boot Ansible readiness and JIT bootstrap") {
		t.Fatalf("a failing bootstrapper = %v", err)
	}
	if !strings.Contains(err.Error(), "JIT credential channel unavailable") {
		t.Fatalf("the bootstrapper's own reason was dropped: %v", err)
	}
}

// And the receipt it returns is held to this reservation and to this call:
// a receipt for another guest, one whose evidence is unusable, or a live one
// replayed from an earlier bootstrap of the same reservation are all refused
// before anything is recorded.
func TestABootstrapReceiptIsHeldToThisReservationAndThisCall(t *testing.T) {
	h, ledger, _, bootstrap, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	good := bootstrap.receipts[vm.ID]
	if good.ReservationID != vm.ID {
		t.Fatalf("fixture receipt is for %q, want the reservation under test", good.ReservationID)
	}

	otherGuest := good
	otherGuest.VMID = good.VMID + 1
	otherCommit := good
	otherCommit.CommitSHA = strings.Repeat("e", 40)
	notARa8Account := good
	notARa8Account.ServiceAccount = "root"
	foreignOS := good
	foreignOS.GuestOS = "plan9"
	unusableEvidence := good
	unusableEvidence.EvidenceID = "not-an-id"
	shortDigest := good
	shortDigest.JITConfigSHA256 = "abc123"
	expiredCredential := good
	expiredCredential.JITConfigExpiresAt = time.Now().Add(-time.Minute)

	for name, receipt := range map[string]BootstrapReceipt{
		"another guest":             otherGuest,
		"another commit":            otherCommit,
		"a privileged account":      notARa8Account,
		"an unsupported guest OS":   foreignOS,
		"unusable evidence":         unusableEvidence,
		"a truncated digest":        shortDigest,
		"an expired JIT credential": expiredCredential,
	} {
		bootstrap.receipts[vm.ID] = receipt
		if err := h.bootstrapRunning(context.Background(), vm); err == nil ||
			!strings.Contains(err.Error(), "lacks fresh identity-bound readiness evidence") {
			t.Fatalf("%s = %v", name, err)
		}
	}

	// A receipt prepared long before this call is the replay the bracket
	// exists to catch, and it is named as a replay rather than lumped in
	// with a malformed one.
	replayed := good
	replayed.PreparedAt = time.Now().Add(-time.Hour)
	bootstrap.receipts[vm.ID] = replayed
	err := h.bootstrapRunning(context.Background(), vm)
	if err == nil || !strings.Contains(err.Error(), "before the bootstrap was asked for") {
		t.Fatalf("a replayed receipt = %v", err)
	}
	if !strings.Contains(err.Error(), vm.ID) {
		t.Fatalf("the replay refusal does not name the reservation: %v", err)
	}
}
