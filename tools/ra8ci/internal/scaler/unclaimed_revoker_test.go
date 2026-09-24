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

type fakeRegistrations struct {
	calls   []github.RunnerRef
	removed bool
	err     error
}

func (f *fakeRegistrations) Revoke(_ context.Context, ref github.RunnerRef) (bool, error) {
	f.calls = append(f.calls, ref)
	return f.removed, f.err
}

type fakeGuests struct {
	calls []string
	err   error
}

func (f *fakeGuests) DestroyUnclaimedGuest(_ context.Context, vm store.RunnerVM) error {
	f.calls = append(f.calls, vm.ID)
	return f.err
}

type fakeLeases struct {
	calls []string
	err   error
}

func (f *fakeLeases) ReleaseReservationLease(_ context.Context, vm store.RunnerVM) error {
	f.calls = append(f.calls, vm.ID)
	return f.err
}

type fakeCloser struct {
	calls  []string
	actor  string
	result store.RunnerVM
	err    error
}

func (f *fakeCloser) ReleaseUnclaimedRunnerVM(_ context.Context, actor, id string) (store.RunnerVM, error) {
	f.calls = append(f.calls, id)
	f.actor = actor
	if f.err != nil {
		return store.RunnerVM{}, f.err
	}
	result := f.result
	if result.State == "" {
		result.State = "released"
	}
	result.ID = id
	return result, nil
}

func revocationFixture(t *testing.T) (*UnclaimedRevocation, *fakeRegistrations, *fakeGuests, *fakeLeases, *fakeCloser) {
	t.Helper()
	registrations := &fakeRegistrations{removed: true}
	guests := &fakeGuests{}
	leases := &fakeLeases{}
	closer := &fakeCloser{}
	revocation, err := NewUnclaimedRevocation("reaper", registrations, guests, leases, closer)
	if err != nil {
		t.Fatalf("new unclaimed revocation: %v", err)
	}
	return revocation, registrations, guests, leases, closer
}

func unclaimedVM(state string) store.RunnerVM {
	vm := store.RunnerVM{
		ID:                 "b0a1c2d3-0000-4000-8000-000000000001",
		State:              state,
		Generation:         3,
		ExternalRunnerID:   4821,
		ExternalRunnerName: "ra8ci-99887766-1",
		UnclaimedDeadline:  time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC),
	}
	vm.VMID = 9012
	return vm
}

func walk(t *testing.T, revocation *UnclaimedRevocation, vm store.RunnerVM) error {
	t.Helper()
	ctx := context.Background()
	for _, step := range []func(context.Context, store.RunnerVM) error{
		revocation.RevokeRegistration, revocation.DestroyGuest,
		revocation.ReleaseLease, revocation.AbandonAttempt,
	} {
		if err := step(ctx, vm); err != nil {
			return err
		}
	}
	return nil
}

// The reaper calls these four methods by name through its own interface. If
// this file and unclaimed_reaper.go ever disagree about the set, the build
// says so here rather than at whatever wires them together.
func TestUnclaimedRevocationSatisfiesTheReaper(t *testing.T) {
	revocation, _, _, _, _ := revocationFixture(t)
	var revoker UnclaimedRevoker = revocation
	if revoker == nil {
		t.Fatal("revocation does not satisfy UnclaimedRevoker")
	}
	if len(UnclaimedSteps()) != 4 {
		t.Fatalf("sequence has %d steps, this file implements 4", len(UnclaimedSteps()))
	}
}

func TestUnclaimedRevocationWalksEveryStep(t *testing.T) {
	revocation, registrations, guests, leases, closer := revocationFixture(t)
	vm := unclaimedVM("registered")
	if err := walk(t, revocation, vm); err != nil {
		t.Fatalf("walk: %v", err)
	}
	if len(registrations.calls) != 1 || registrations.calls[0].ID != 4821 ||
		registrations.calls[0].Name != "ra8ci-99887766-1" {
		t.Fatalf("registration step got %+v", registrations.calls)
	}
	if len(guests.calls) != 1 || len(leases.calls) != 1 || len(closer.calls) != 1 {
		t.Fatalf("guest=%v lease=%v close=%v", guests.calls, leases.calls, closer.calls)
	}
	if closer.actor != "reaper" {
		t.Fatalf("closed as %q, want the configured actor", closer.actor)
	}
}

// A reservation cancelled before its credential was ever registered is the
// ordinary case, not an anomaly: no forge call, and the sequence still ends
// with the reservation closed.
func TestUnclaimedRevocationSpendsNoForgeCallWithoutARegistration(t *testing.T) {
	revocation, registrations, _, _, closer := revocationFixture(t)
	vm := unclaimedVM("reserved")
	vm.ExternalRunnerID, vm.ExternalRunnerName = 0, ""
	if err := walk(t, revocation, vm); err != nil {
		t.Fatalf("walk: %v", err)
	}
	if len(registrations.calls) != 1 || registrations.calls[0].Registered() {
		t.Fatalf("passed a registered ref: %+v", registrations.calls)
	}
	if len(closer.calls) != 1 {
		t.Fatalf("reservation not closed: %v", closer.calls)
	}
}

// Nothing is cloned until the clone operation begins, so a reservation still
// at reserved must not reach the hypervisor at all.
func TestUnclaimedRevocationDestroysOnlyGuestsThatExist(t *testing.T) {
	for _, state := range []string{"reserved", "released"} {
		revocation, _, guests, _, _ := revocationFixture(t)
		if err := revocation.DestroyGuest(context.Background(), unclaimedVM(state)); err != nil {
			t.Fatalf("%s: %v", state, err)
		}
		if len(guests.calls) != 0 {
			t.Fatalf("%s reached the hypervisor: %v", state, guests.calls)
		}
	}
	for _, state := range []string{"cloning", "stopped", "starting", "running",
		"registered", "draining", "stopping", "deleting"} {
		revocation, _, guests, _, _ := revocationFixture(t)
		if err := revocation.DestroyGuest(context.Background(), unclaimedVM(state)); err != nil {
			t.Fatalf("%s: %v", state, err)
		}
		if len(guests.calls) != 1 {
			t.Fatalf("%s did not reach the hypervisor", state)
		}
	}
}

// The ledger and the forge disagreeing about which runner this reservation
// holds is the one case where carrying on would destroy a guest whose
// registration is still live.
func TestUnclaimedRevocationStopsOnAForeignRunner(t *testing.T) {
	revocation, registrations, guests, leases, closer := revocationFixture(t)
	registrations.err = github.ErrForeignRunner
	err := walk(t, revocation, unclaimedVM("registered"))
	if !errors.Is(err, github.ErrForeignRunner) {
		t.Fatalf("walk returned %v, want a foreign runner refusal", err)
	}
	if len(guests.calls) != 0 || len(leases.calls) != 0 || len(closer.calls) != 0 {
		t.Fatalf("acted past the refusal: guest=%v lease=%v close=%v",
			guests.calls, leases.calls, closer.calls)
	}
}

// A truncated external id names a different runner, and this is the step
// whose whole job is not removing somebody else's.
func TestUnclaimedRevocationRefusesAnOutOfRangeRunnerID(t *testing.T) {
	revocation, registrations, _, _, _ := revocationFixture(t)
	vm := unclaimedVM("registered")
	vm.ExternalRunnerID = 1 << 40
	if err := revocation.RevokeRegistration(context.Background(), vm); err == nil {
		t.Fatal("accepted a runner id outside the forge's range")
	}
	if len(registrations.calls) != 0 {
		t.Fatalf("called the forge with %+v", registrations.calls)
	}
}

func TestUnclaimedRevocationStopsAtAFailedGuestDestroy(t *testing.T) {
	revocation, _, guests, leases, closer := revocationFixture(t)
	guests.err = errors.New("proxmox unreachable")
	err := walk(t, revocation, unclaimedVM("running"))
	if err == nil {
		t.Fatal("walked past a failed destroy")
	}
	if len(leases.calls) != 0 || len(closer.calls) != 0 {
		t.Fatalf("acted past the failure: lease=%v close=%v", leases.calls, closer.calls)
	}
}

func TestUnclaimedRevocationStopsAtAFailedLeaseRelease(t *testing.T) {
	revocation, _, _, leases, closer := revocationFixture(t)
	leases.err = errors.New("bench unreachable")
	if err := walk(t, revocation, unclaimedVM("running")); err == nil {
		t.Fatal("walked past a failed lease release")
	}
	if len(closer.calls) != 0 {
		t.Fatalf("closed the reservation anyway: %v", closer.calls)
	}
}

// The job claiming its runner between the reaper's re-check and the commit
// is exactly the race the store refuses. Nothing has been torn down by then,
// because the earlier steps only ran on a reservation that was still
// unclaimed when they did.
func TestUnclaimedRevocationSurfacesAClaimUnderTheCommit(t *testing.T) {
	revocation, _, _, _, closer := revocationFixture(t)
	closer.err = store.ErrConflict
	err := revocation.AbandonAttempt(context.Background(), unclaimedVM("running"))
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("abandon returned %v, want a conflict", err)
	}
}

// A store that answers without releasing the row would leave it in the queue
// while the pass counted it reaped, so the commit checks what it got back.
func TestUnclaimedRevocationRefusesAnUnreleasedRow(t *testing.T) {
	revocation, _, _, _, closer := revocationFixture(t)
	closer.result = store.RunnerVM{State: "running"}
	err := revocation.AbandonAttempt(context.Background(), unclaimedVM("running"))
	if !errors.Is(err, store.ErrConflict) {
		t.Fatalf("abandon returned %v, want a conflict", err)
	}
}

// A resumed pass runs the whole sequence again against work that is already
// done. Every step has to be a success for the reservation to leave the
// queue at all.
func TestUnclaimedRevocationIsIdempotent(t *testing.T) {
	revocation, registrations, guests, leases, closer := revocationFixture(t)
	registrations.removed = false
	vm := unclaimedVM("deleting")
	for round := range 3 {
		if err := walk(t, revocation, vm); err != nil {
			t.Fatalf("round %d: %v", round, err)
		}
	}
	if len(registrations.calls) != 3 || len(guests.calls) != 3 ||
		len(leases.calls) != 3 || len(closer.calls) != 3 {
		t.Fatalf("steps did not all run three times: %d %d %d %d",
			len(registrations.calls), len(guests.calls), len(leases.calls), len(closer.calls))
	}
}

func TestNewUnclaimedRevocationRefusesAPartialWiring(t *testing.T) {
	registrations, guests := &fakeRegistrations{}, &fakeGuests{}
	leases, closer := &fakeLeases{}, &fakeCloser{}
	cases := map[string]func() (*UnclaimedRevocation, error){
		"no actor": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation("", registrations, guests, leases, closer)
		},
		"no forge": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation("reaper", nil, guests, leases, closer)
		},
		"no hypervisor": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation("reaper", registrations, nil, leases, closer)
		},
		"no bench": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation("reaper", registrations, guests, nil, closer)
		},
		"no ledger": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation("reaper", registrations, guests, leases, nil)
		},
		"oversized actor": func() (*UnclaimedRevocation, error) {
			return NewUnclaimedRevocation(string(make([]byte, 257)), registrations, guests, leases, closer)
		},
	}
	for name, build := range cases {
		if _, err := build(); err == nil {
			t.Fatalf("%s: accepted", name)
		}
	}
}

func TestUnclaimedRevocationRefusesAZeroReceiver(t *testing.T) {
	var revocation *UnclaimedRevocation
	vm := unclaimedVM("running")
	ctx := context.Background()
	for name, step := range map[string]func(context.Context, store.RunnerVM) error{
		StepRevokeRegistration: revocation.RevokeRegistration,
		StepDestroyGuest:       revocation.DestroyGuest,
		StepReleaseLease:       revocation.ReleaseLease,
		StepAbandonAttempt:     revocation.AbandonAttempt,
	} {
		if err := step(ctx, vm); err == nil {
			t.Fatalf("%s: a zero revocation reported success", name)
		}
	}
}
