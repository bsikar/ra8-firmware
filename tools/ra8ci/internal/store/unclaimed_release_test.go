// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"
	"time"
)

func TestUnclaimedReleaseIsIdempotentOnceReleased(t *testing.T) {
	released := RunnerVM{State: "released"}
	done, err := unclaimedRelease(released)
	if err != nil || !done {
		t.Fatalf("a released reservation is not settled: done=%v err=%v", done, err)
	}
	// Even one the reaper would otherwise refuse: a released row has
	// nothing left to tear down, whatever else is recorded on it.
	claimedAt := time.Now().UTC()
	both := RunnerVM{State: "released", ClaimedAt: &claimedAt, UnknownOutcome: true}
	if done, err := unclaimedRelease(both); err != nil || !done {
		t.Fatalf("released outranks every other refusal: done=%v err=%v", done, err)
	}
}

func TestUnclaimedReleaseRefusesAClaimedReservation(t *testing.T) {
	claimedAt := time.Now().UTC()
	vm := RunnerVM{State: "registered", ClaimedAt: &claimedAt}
	done, err := unclaimedRelease(vm)
	if done || !errors.Is(err, ErrConflict) {
		t.Fatalf("claimed reservation released: done=%v err=%v", done, err)
	}
	if UnclaimedReservation(vm) {
		t.Fatal("the queue predicate and the release guard disagree about a claimed row")
	}
}

func TestUnclaimedReleaseRefusesAnOutstandingOperation(t *testing.T) {
	vm := RunnerVM{State: "cloning", UnknownOutcome: true, CurrentOperationID: "op-1"}
	done, err := unclaimedRelease(vm)
	if done || !errors.Is(err, ErrConflict) {
		t.Fatalf("released underneath a live operation: done=%v err=%v", done, err)
	}
	// The queue still offers it, which is the point of refusing here: the
	// row stays in the batch and the next pass retries once Proxmox has
	// reported, rather than the reaper deciding the guest's fate itself.
	if !UnclaimedReservation(vm) {
		t.Fatal("an unresolved reservation left the reaper queue")
	}
}

func TestUnclaimedReleaseAllowsEveryLiveState(t *testing.T) {
	// Every state the guest lifecycle can be sitting in when nobody took
	// the credential. Exercising the whole set is what stops a new state
	// being added to the schema and quietly falling out of the reaper.
	for _, state := range []string{"reserved", "cloning", "stopped", "starting",
		"running", "registered", "draining", "stopping", "deleting"} {
		vm := RunnerVM{State: state}
		done, err := unclaimedRelease(vm)
		if done || err != nil {
			t.Fatalf("%s: done=%v err=%v", state, done, err)
		}
		if !UnclaimedReservation(vm) {
			t.Fatalf("%s: releasable but not in the queue", state)
		}
	}
}

func TestReleaseUnclaimedRunnerVMRejectsBadArguments(t *testing.T) {
	// A well formed reservation id, so every failure below is the
	// argument the case is actually about.
	const reservationID = "0f0f6f3a-2c1d-4d8e-9b2a-6d5c4b3a2190"
	var nilStore *Store
	if _, err := nilStore.ReleaseUnclaimedRunnerVM(t.Context(), "reaper", reservationID); !errors.Is(err, ErrInvalid) {
		t.Fatalf("nil store: %v", err)
	}
	var s Store
	for name, call := range map[string]func() error{
		"empty actor": func() error {
			_, err := s.ReleaseUnclaimedRunnerVM(t.Context(), "", reservationID)
			return err
		},
		"unparsable reservation": func() error {
			_, err := s.ReleaseUnclaimedRunnerVM(t.Context(), "reaper", "not-a-uuid")
			return err
		},
	} {
		if err := call(); !errors.Is(err, ErrInvalid) {
			t.Fatalf("%s: %v", name, err)
		}
	}
}
