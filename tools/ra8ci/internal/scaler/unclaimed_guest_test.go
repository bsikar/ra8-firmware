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

// fakeRevocation stands in for the forge. removed is what Revoke answers, so
// a test can present a registration that is gone (the ordinary case) or one
// that is unexpectedly still live.
type fakeRevocation struct {
	calls   int
	removed bool
	err     error
	refs    []github.RunnerRef
}

func (f *fakeRevocation) Revoke(_ context.Context, ref github.RunnerRef) (bool, error) {
	f.calls++
	f.refs = append(f.refs, ref)
	return f.removed, f.err
}

func claimedNow() *time.Time {
	at := time.Now().UTC()
	return &at
}

func TestUnclaimedDestroyableGuardsEveryReservationShape(t *testing.T) {
	for _, tc := range []struct {
		name    string
		vm      store.RunnerVM
		done    bool
		wantErr error
	}{
		{"released is already done", store.RunnerVM{ID: "a", State: "released"}, true, nil},
		{"released outranks a claim", store.RunnerVM{ID: "a", State: "released", ClaimedAt: claimedNow()}, true, nil},
		{"claimed is never torn down", store.RunnerVM{ID: "a", State: "running", ClaimedAt: claimedNow()}, false, store.ErrConflict},
		{"unresolved stays queued", store.RunnerVM{ID: "a", State: "running", UnknownOutcome: true}, false, store.ErrConflict},
		{"reserved never cloned a guest", store.RunnerVM{ID: "a", State: "reserved"}, true, nil},
		{"cloning may have a guest", store.RunnerVM{ID: "a", State: "cloning"}, false, nil},
		{"running has one", store.RunnerVM{ID: "a", State: "running"}, false, nil},
		{"registered has one", store.RunnerVM{ID: "a", State: "registered"}, false, nil},
		{"stopped has one", store.RunnerVM{ID: "a", State: "stopped"}, false, nil},
		{"draining has one", store.RunnerVM{ID: "a", State: "draining"}, false, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			done, err := unclaimedDestroyable(tc.vm)
			if done != tc.done {
				t.Fatalf("done = %v, want %v", done, tc.done)
			}
			if tc.wantErr == nil && err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if tc.wantErr != nil && !errors.Is(err, tc.wantErr) {
				t.Fatalf("error = %v, want %v", err, tc.wantErr)
			}
		})
	}
}

// A claimed reservation must not even produce evidence, let alone spend it.
func TestUnclaimedSafetyRefusesAClaimedReservation(t *testing.T) {
	if _, err := unclaimedSafety(store.RunnerVM{ID: "a", ClaimedAt: claimedNow()}, time.Now()); !errors.Is(err, store.ErrConflict) {
		t.Fatalf("claimed reservation produced evidence: %v", err)
	}
	if _, err := unclaimedSafety(store.RunnerVM{ID: "a"}, time.Time{}); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("evidence without a clock: %v", err)
	}
	proof, err := unclaimedSafety(store.RunnerVM{ID: "a", ExternalRunnerID: 77}, time.Now().UTC())
	if err != nil {
		t.Fatal(err)
	}
	if !proof.Drained || !proof.NoActiveJob || !proof.RunnerDeregistered ||
		proof.ExternalRunnerID != 77 || !store.ValidID(proof.EvidenceID) {
		t.Fatalf("unclaimed evidence is not the stated argument: %+v", proof)
	}
	if proof.ApprovalID != "" || proof.ExpectedConfigDigest != "" {
		t.Fatalf("stop evidence carries destroy authority: %+v", proof)
	}
}

func TestNewUnclaimedDestroyerRefusesAPartialWiring(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	if _, err := NewUnclaimedDestroyer(nil, &fakeRevocation{}); err == nil {
		t.Fatal("destroyer without a handler")
	}
	if _, err := NewUnclaimedDestroyer(h, nil); err == nil {
		t.Fatal("destroyer without a forge")
	}
	if _, err := h.UnclaimedDestroyer(&fakeRevocation{}); err != nil {
		t.Fatal(err)
	}
}

// expired drives a reservation to a live, registered guest the ordinary way
// and then expires its credential without anyone having claimed it, which is
// the situation the reaper exists for.
func expired(t *testing.T, h *Handler, ledger *memoryLedger, job github.Job) store.RunnerVM {
	t.Helper()
	ctx := context.Background()
	if err := h.Process(ctx, github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}); err != nil {
		t.Fatal(err)
	}
	vm, err := ledger.GetRunnerVMByJob(ctx, 42, job.JobID)
	if err != nil {
		t.Fatal(err)
	}
	if vm.State != "running" || vm.ClaimedAt != nil {
		t.Fatalf("fixture is not an unclaimed running reservation: %+v", vm)
	}
	return vm
}

func TestUnclaimedDestroyerStopsAndDestroysTheGuestNobodyTook(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); err != nil {
		t.Fatal(err)
	}
	after, err := ledger.GetRunnerVM(context.Background(), vm.ID)
	if err != nil {
		t.Fatal(err)
	}
	if after.State != "released" || !after.CleanupRequested {
		t.Fatalf("guest not destroyed: %+v", after)
	}
	if forge.calls != 1 {
		t.Fatalf("forge asked %d times, want exactly one absence check", forge.calls)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != 1 || fake.deleteCalls != 1 || fake.exists {
		t.Fatalf("hypervisor calls stop=%d delete=%d exists=%v", fake.stopCalls, fake.deleteCalls, fake.exists)
	}
}

// Second pass over a reservation the first one finished: nothing is asked of
// the forge or the hypervisor again.
func TestUnclaimedDestroyerIsIdempotentOnAReleasedReservation(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	vm := expired(t, h, ledger, job)
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	stops, deletes := fake.stopCalls, fake.deleteCalls
	fake.mu.Unlock()
	if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); err != nil {
		t.Fatalf("resumed pass failed on a finished reservation: %v", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.stopCalls != stops || fake.deleteCalls != deletes || forge.calls != 1 {
		t.Fatalf("second pass mutated again: stop=%d delete=%d forge=%d", fake.stopCalls, fake.deleteCalls, forge.calls)
	}
}

func TestUnclaimedDestroyerRefusesAClaimedOrLiveRegistration(t *testing.T) {
	t.Run("claimed under the reaper", func(t *testing.T) {
		h, ledger, fake, _, job := testHarness(t)
		vm := expired(t, h, ledger, job)
		ledger.mu.Lock()
		ledger.vm.ClaimedAt = claimedNow()
		ledger.mu.Unlock()
		forge := &fakeRevocation{}
		destroyer, err := h.UnclaimedDestroyer(forge)
		if err != nil {
			t.Fatal(err)
		}
		if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); !errors.Is(err, store.ErrConflict) {
			t.Fatalf("claimed reservation torn down: %v", err)
		}
		fake.mu.Lock()
		defer fake.mu.Unlock()
		if forge.calls != 0 || fake.stopCalls != 0 || fake.deleteCalls != 0 {
			t.Fatalf("spent calls on a claimed reservation: forge=%d stop=%d delete=%d", forge.calls, fake.stopCalls, fake.deleteCalls)
		}
	})
	t.Run("registration still live", func(t *testing.T) {
		h, ledger, fake, _, job := testHarness(t)
		vm := expired(t, h, ledger, job)
		forge := &fakeRevocation{removed: true}
		destroyer, err := h.UnclaimedDestroyer(forge)
		if err != nil {
			t.Fatal(err)
		}
		err = destroyer.DestroyUnclaimedGuest(context.Background(), vm)
		if !errors.Is(err, ErrUnclaimedIncomplete) {
			t.Fatalf("destroyed through a live registration: %v", err)
		}
		fake.mu.Lock()
		defer fake.mu.Unlock()
		if fake.stopCalls != 0 || fake.deleteCalls != 0 {
			t.Fatalf("hypervisor touched: stop=%d delete=%d", fake.stopCalls, fake.deleteCalls)
		}
	})
	t.Run("forge unreachable", func(t *testing.T) {
		h, ledger, fake, _, job := testHarness(t)
		vm := expired(t, h, ledger, job)
		forge := &fakeRevocation{err: errors.New("forge down")}
		destroyer, err := h.UnclaimedDestroyer(forge)
		if err != nil {
			t.Fatal(err)
		}
		if err := destroyer.DestroyUnclaimedGuest(context.Background(), vm); err == nil {
			t.Fatal("unreachable forge treated as an absent registration")
		}
		fake.mu.Lock()
		defer fake.mu.Unlock()
		if fake.stopCalls != 0 || fake.deleteCalls != 0 {
			t.Fatalf("hypervisor touched: stop=%d delete=%d", fake.stopCalls, fake.deleteCalls)
		}
	})
}

// A reservation that never cloned anything is a success with no calls at all,
// which is the ordinary shape of a credential cancelled early.
func TestUnclaimedDestroyerSkipsAReservationWithNoGuest(t *testing.T) {
	h, _, fake, _, _ := testHarness(t)
	forge := &fakeRevocation{}
	destroyer, err := h.UnclaimedDestroyer(forge)
	if err != nil {
		t.Fatal(err)
	}
	id, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	ledger, ok := h.ledger.(*memoryLedger)
	if !ok {
		t.Fatalf("harness ledger is %T", h.ledger)
	}
	ledger.mu.Lock()
	ledger.vm = store.RunnerVM{ID: id, State: "reserved", Generation: 1,
		RunnerVMInput: store.RunnerVMInput{ScaleSetID: 42, VMID: 9000}}
	ledger.mu.Unlock()
	if err := destroyer.DestroyUnclaimedGuest(context.Background(), store.RunnerVM{ID: id, VMID: 9000}); err != nil {
		t.Fatal(err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if forge.calls != 0 || fake.stopCalls != 0 || fake.deleteCalls != 0 {
		t.Fatalf("spent calls on a reservation with no guest: forge=%d stop=%d delete=%d", forge.calls, fake.stopCalls, fake.deleteCalls)
	}
}

// The destroyer is the missing half of the revoker's second step: wiring one
// into the other must satisfy the seam without an adapter.
func TestUnclaimedDestroyerSatisfiesTheRevokerSeam(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	destroyer, err := h.UnclaimedDestroyer(&fakeRevocation{})
	if err != nil {
		t.Fatal(err)
	}
	var seam GuestDestroyer = destroyer
	if seam == nil {
		t.Fatal("destroyer does not satisfy GuestDestroyer")
	}
}
