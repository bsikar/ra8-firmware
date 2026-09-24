// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"
	"math"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The concrete revoker behind the unclaimed sequence. unclaimed_reaper.go
// owns the order and the accounting and knows nothing about how any step
// commits; this file is where the four steps meet the three systems that
// actually perform them: the forge, the hypervisor, the bench, and finally
// the ledger, which is the only one whose write takes the reservation out of
// the queue.
//
// Two decisions are worth stating here rather than in a comment at a call
// site. A step with nothing to do is a success, not a skip: a reservation
// cancelled before its credential was ever registered has no registration to
// remove and no guest to destroy, and that is the ordinary case, not an
// anomaly. A step that cannot tell whether it has anything to do is a
// failure: the row stays in the queue and the next pass walks the sequence
// from the top, which is cheaper than guessing once.

// RegistrationRevocation is the forge half of the first step.
// *github.RegistrationRevoker satisfies it.
type RegistrationRevocation interface {
	Revoke(context.Context, github.RunnerRef) (bool, error)
}

var _ RegistrationRevocation = (*github.RegistrationRevoker)(nil)

// GuestDestroyer destroys the guest a reservation cloned, if it got that far.
// It must be idempotent: a resumed pass asks again for a guest that is
// already gone, and that has to be a success.
type GuestDestroyer interface {
	DestroyUnclaimedGuest(context.Context, store.RunnerVM) error
}

// LeaseReleaser releases any bench lease held on behalf of the reservation.
// A reservation nobody claimed usually holds none, so the common answer is a
// no-op; the seam exists because only the bench can say so.
type LeaseReleaser interface {
	ReleaseReservationLease(context.Context, store.RunnerVM) error
}

// ReservationCloser is the commit point. *store.Store satisfies it.
type ReservationCloser interface {
	ReleaseUnclaimedRunnerVM(context.Context, string, string) (store.RunnerVM, error)
}

var _ ReservationCloser = (*store.Store)(nil)

// UnclaimedRevocation performs the four steps the reaper walks.
type UnclaimedRevocation struct {
	actor         string
	registrations RegistrationRevocation
	guests        GuestDestroyer
	leases        LeaseReleaser
	reservations  ReservationCloser
}

var _ UnclaimedRevoker = (*UnclaimedRevocation)(nil)

// NewUnclaimedRevocation refuses a revoker missing any of its four systems.
// A partially wired revoker would run the steps it has and report the
// reservation reaped, which is the one outcome worse than not running.
func NewUnclaimedRevocation(actor string, registrations RegistrationRevocation, guests GuestDestroyer, leases LeaseReleaser, reservations ReservationCloser) (*UnclaimedRevocation, error) {
	if actor == "" || len(actor) > 256 {
		return nil, errors.New("unclaimed revocation needs an actor")
	}
	if registrations == nil || guests == nil || leases == nil || reservations == nil {
		return nil, errors.New("unclaimed revocation needs a forge, a hypervisor, a bench and a ledger")
	}
	return &UnclaimedRevocation{actor: actor, registrations: registrations,
		guests: guests, leases: leases, reservations: reservations}, nil
}

// runnerRef is what the ledger believes was registered for this reservation.
// An external id too large for the forge's own int is refused rather than
// truncated: a truncated id names a different runner, and this is the one
// step whose whole job is not removing somebody else's.
func runnerRef(vm store.RunnerVM) (github.RunnerRef, error) {
	if vm.ExternalRunnerID < 0 || vm.ExternalRunnerID > math.MaxInt32 {
		return github.RunnerRef{}, fmt.Errorf("reservation %s holds runner id %d, outside the forge's range",
			vm.ID, vm.ExternalRunnerID)
	}
	return github.RunnerRef{ID: int(vm.ExternalRunnerID), Name: vm.ExternalRunnerName}, nil
}

// RevokeRegistration removes the registration nobody used. A reservation that
// never registered spends no forge call at all.
//
// github.ErrForeignRunner stops the sequence on purpose. It means the ledger
// row and the forge disagree about which runner this reservation holds, and
// the next three steps would act on that same disagreement: destroying a
// guest and closing a reservation whose registration is still live is exactly
// the failure the ordering exists to prevent. The row stays queued and an
// operator sees it in the pass report.
func (r *UnclaimedRevocation) RevokeRegistration(ctx context.Context, vm store.RunnerVM) error {
	if r == nil || r.registrations == nil || ctx == nil {
		return errors.New("invalid unclaimed revocation")
	}
	ref, err := runnerRef(vm)
	if err != nil {
		return err
	}
	if _, err := r.registrations.Revoke(ctx, ref); err != nil {
		return fmt.Errorf("revoke registration for reservation %s: %w", vm.ID, err)
	}
	return nil
}

// DestroyGuest destroys the guest, when the reservation ever had one. The
// state is the ledger's answer to "was anything cloned", and a reservation
// still at reserved never reaches the hypervisor.
func (r *UnclaimedRevocation) DestroyGuest(ctx context.Context, vm store.RunnerVM) error {
	if r == nil || r.guests == nil || ctx == nil {
		return errors.New("invalid unclaimed revocation")
	}
	if !store.UnclaimedGuestExists(vm) {
		return nil
	}
	if err := r.guests.DestroyUnclaimedGuest(ctx, vm); err != nil {
		return fmt.Errorf("destroy guest %d for reservation %s: %w", vm.VMID, vm.ID, err)
	}
	return nil
}

// ReleaseLease hands the reservation to the bench. Whether a lease is held is
// the bench's fact, not the ledger's, so this always asks rather than reading
// a state that would only be a stale copy of the answer.
func (r *UnclaimedRevocation) ReleaseLease(ctx context.Context, vm store.RunnerVM) error {
	if r == nil || r.leases == nil || ctx == nil {
		return errors.New("invalid unclaimed revocation")
	}
	if err := r.leases.ReleaseReservationLease(ctx, vm); err != nil {
		return fmt.Errorf("release lease for reservation %s: %w", vm.ID, err)
	}
	return nil
}

// AbandonAttempt closes the reservation, which is what takes it out of the
// reaper's queue. It runs last because everything before it can be retried
// from the queue and this cannot: once the row is released the pass has no
// handle on it.
//
// store.ErrConflict here is the job claiming its runner between the reaper's
// re-check and this write. Nothing is torn down, the error surfaces, and the
// dispatch reaper owns the attempt from then on.
func (r *UnclaimedRevocation) AbandonAttempt(ctx context.Context, vm store.RunnerVM) error {
	if r == nil || r.reservations == nil || ctx == nil {
		return errors.New("invalid unclaimed revocation")
	}
	released, err := r.reservations.ReleaseUnclaimedRunnerVM(ctx, r.actor, vm.ID)
	if err != nil {
		return fmt.Errorf("abandon reservation %s: %w", vm.ID, err)
	}
	if released.State != "released" {
		return fmt.Errorf("%w: reservation %s is %s after release", store.ErrConflict, vm.ID, released.State)
	}
	return nil
}
