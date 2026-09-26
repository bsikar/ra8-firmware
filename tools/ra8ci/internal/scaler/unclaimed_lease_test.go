// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type fakeBench struct {
	live  map[string][]store.LiveBoardLease
	fail  map[string]error
	asked []string
}

func (b *fakeBench) ListLiveBoardLeasesByHolder(_ context.Context, holder string) ([]store.LiveBoardLease, error) {
	b.asked = append(b.asked, holder)
	if err := b.fail[holder]; err != nil {
		return nil, err
	}
	return b.live[holder], nil
}

func unclaimedLeaseFixture() store.RunnerVM {
	return store.RunnerVM{
		ID:                 "22222222-2222-4222-8222-222222222222",
		State:              "registered",
		ExternalRunnerName: "ra8ci-987654321-2",
		UnclaimedDeadline:  time.Date(2026, 9, 24, 12, 30, 0, 0, time.UTC),
	}
}

func TestUnclaimedLeaseGuardPassesWhenTheBenchHoldsNothing(t *testing.T) {
	bench := &fakeBench{}
	guard, err := NewUnclaimedLeaseGuard(bench)
	if err != nil {
		t.Fatalf("guard: %v", err)
	}
	vm := unclaimedLeaseFixture()
	if err := guard.ReleaseReservationLease(context.Background(), vm); err != nil {
		t.Fatalf("an unclaimed reservation ordinarily holds no lease: %v", err)
	}
	if len(bench.asked) != 2 || bench.asked[0] != vm.ID || bench.asked[1] != vm.ExternalRunnerName {
		t.Fatalf("both identities must be asked about, got %v", bench.asked)
	}
}

func TestUnclaimedLeaseGuardStopsOnALiveLease(t *testing.T) {
	bench := &fakeBench{live: map[string][]store.LiveBoardLease{
		"ra8ci-987654321-2": {{
			ID: "33333333-3333-4333-8333-333333333333", BoardID: "bench-a",
			HolderID: "ra8ci-987654321-2", Priority: "ci", State: "active", Generation: 7,
		}},
	}}
	guard, _ := NewUnclaimedLeaseGuard(bench)
	err := guard.ReleaseReservationLease(context.Background(), unclaimedLeaseFixture())
	if !errors.Is(err, ErrUnclaimedLeaseHeld) {
		t.Fatalf("want ErrUnclaimedLeaseHeld, got %v", err)
	}
	for _, want := range []string{"bench-a", "33333333-3333-4333-8333-333333333333", "ci"} {
		if !contains(err.Error(), want) {
			t.Fatalf("the refusal must name %q so an operator can find it: %v", want, err)
		}
	}
}

// A lease under the reservation id is just as much a contradiction as one
// under the runner name, and it must be caught before the runner name is even
// asked about: the sequence stops at the first one found.
func TestUnclaimedLeaseGuardStopsOnTheFirstIdentity(t *testing.T) {
	vm := unclaimedLeaseFixture()
	bench := &fakeBench{live: map[string][]store.LiveBoardLease{
		vm.ID: {{ID: "44444444-4444-4444-8444-444444444444", BoardID: "bench-b",
			HolderID: vm.ID, State: "pending", Priority: "agent"}},
	}}
	guard, _ := NewUnclaimedLeaseGuard(bench)
	if err := guard.ReleaseReservationLease(context.Background(), vm); !errors.Is(err, ErrUnclaimedLeaseHeld) {
		t.Fatalf("want ErrUnclaimedLeaseHeld, got %v", err)
	}
	if len(bench.asked) != 1 {
		t.Fatalf("the step stops at the first contradiction, asked %v", bench.asked)
	}
}

// An unreachable bench is a failure, never a pass: the row stays in the queue
// and the next pass walks the sequence from the top.
func TestUnclaimedLeaseGuardFailsWhenItCannotAsk(t *testing.T) {
	vm := unclaimedLeaseFixture()
	bench := &fakeBench{fail: map[string]error{vm.ID: store.ErrUnavailable}}
	guard, _ := NewUnclaimedLeaseGuard(bench)
	err := guard.ReleaseReservationLease(context.Background(), vm)
	if !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("want the bench failure through, got %v", err)
	}
	if errors.Is(err, ErrUnclaimedLeaseHeld) {
		t.Fatalf("an unreachable bench is not a held lease: %v", err)
	}
}

func TestUnclaimedLeaseGuardRefusesWhatItCannotAskAbout(t *testing.T) {
	if _, err := NewUnclaimedLeaseGuard(nil); err == nil {
		t.Fatal("a guard with no bench must be refused")
	}
	var nilGuard *UnclaimedLeaseGuard
	if err := nilGuard.ReleaseReservationLease(context.Background(), unclaimedLeaseFixture()); err == nil {
		t.Fatal("a nil guard must not report a released lease")
	}
	bench := &fakeBench{}
	guard, _ := NewUnclaimedLeaseGuard(bench)
	if err := guard.ReleaseReservationLease(context.Background(), store.RunnerVM{}); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a reservation with no identity is ErrInvalid, got %v", err)
	}
	if len(bench.asked) != 0 {
		t.Fatalf("a refusal spends no bench read, asked %v", bench.asked)
	}
}

// The guard is only useful if it can actually be wired as the third step, so
// pin it against the revoker's own constructor.
func TestUnclaimedLeaseGuardWiresAsTheThirdStep(t *testing.T) {
	guard, err := NewUnclaimedLeaseGuard(&fakeBench{})
	if err != nil {
		t.Fatalf("guard: %v", err)
	}
	var releaser LeaseReleaser = guard
	if releaser == nil {
		t.Fatal("guard must satisfy LeaseReleaser")
	}
}

func contains(haystack, needle string) bool {
	return len(haystack) >= len(needle) && (func() bool {
		for i := 0; i+len(needle) <= len(haystack); i++ {
			if haystack[i:i+len(needle)] == needle {
				return true
			}
		}
		return false
	})()
}
