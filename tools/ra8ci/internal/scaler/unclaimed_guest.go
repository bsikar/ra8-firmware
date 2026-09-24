// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The concrete GuestDestroyer behind the unclaimed sequence's second step.
// Until this existed the reaper had a seam with nothing on the other side of
// it, so an expired reservation that had already cloned a guest could not be
// taken out of the queue at all.
//
// Destroying a guest on the ordinary path is gated on a job completion: the
// forge says the job finished, the runner observer says the runner drained
// and holds no active job, and that observation becomes the safety evidence
// the ledger demands. A reservation nobody claimed has none of that, because
// no job ever started on it, and waiting for a completion that will never
// arrive is exactly the leak the reaper exists to stop.
//
// So the evidence here is a different argument, and it is worth stating
// plainly rather than burying in a struct literal. "No active job" is the
// ledger's claimed_at column: dispatch sets it when a job takes the runner,
// this reservation's is still NULL, and the row is re-read and re-checked
// immediately before anything is torn down. "Drained" is the same fact seen
// from the other side, since a runner that never accepted work has nothing
// to drain. "Deregistered" is not assumed at all: the forge is asked again,
// here, and a registration that turns out to still be live stops the step.
//
// Everything else the ordinary path insists on is still insisted on. The
// guest is observed before the destroy, it must be stopped, unlocked and
// unprotected, its config digest must be the one the destroy is authorized
// against, and the operator's cleanup approval is still required.

// UnclaimedDestroyer destroys the guest behind a reservation whose runner
// credential expired unused.
type UnclaimedDestroyer struct {
	handler       *Handler
	registrations RegistrationRevocation
}

var _ GuestDestroyer = (*UnclaimedDestroyer)(nil)

// NewUnclaimedDestroyer refuses a destroyer missing either half. The forge
// half is not optional: without it the destroy step would have to assume the
// registration is gone, and a guest destroyed under a live registration is
// the failure the whole step ordering exists to prevent.
func NewUnclaimedDestroyer(handler *Handler, registrations RegistrationRevocation) (*UnclaimedDestroyer, error) {
	if handler == nil || registrations == nil {
		return nil, errors.New("unclaimed destroyer needs a scaler handler and a forge")
	}
	return &UnclaimedDestroyer{handler: handler, registrations: registrations}, nil
}

// UnclaimedDestroyer builds the destroyer from the handler's own wiring.
func (h *Handler) UnclaimedDestroyer(registrations RegistrationRevocation) (*UnclaimedDestroyer, error) {
	return NewUnclaimedDestroyer(h, registrations)
}

// unclaimedDestroyable is the clock-free guard, re-run against a freshly read
// row rather than the one the batch was built from. done means the step has
// nothing left to do and is a success; an error leaves the row in the queue
// for the next pass.
func unclaimedDestroyable(vm store.RunnerVM) (bool, error) {
	if vm.State == "released" {
		return true, nil // already torn down, a resumed pass is normal
	}
	if !store.UnclaimedReservation(vm) {
		return false, fmt.Errorf("%w: reservation %s was claimed at %s", store.ErrConflict,
			vm.ID, claimedAt(vm))
	}
	if vm.UnknownOutcome {
		// The same policy as the release step: a reservation whose last
		// hypervisor mutation nobody heard the outcome of is reconciled
		// first, by the pass that owns reconciliation. It stays queued.
		return false, fmt.Errorf("%w: reservation %s has an unresolved operation", store.ErrConflict, vm.ID)
	}
	if !store.UnclaimedGuestExists(vm) {
		return true, nil // nothing was ever cloned
	}
	return false, nil
}

func claimedAt(vm store.RunnerVM) string {
	if vm.ClaimedAt == nil {
		return "an unknown time"
	}
	return vm.ClaimedAt.UTC().Format(time.RFC3339)
}

// DestroyUnclaimedGuest walks the guest from wherever the reservation left it
// to gone: request cleanup, stop it if it is still up, then destroy it. It is
// idempotent at every point, because the reaper resumes a half-finished
// reservation by running the whole sequence again.
func (d *UnclaimedDestroyer) DestroyUnclaimedGuest(ctx context.Context, vm store.RunnerVM) error {
	if d == nil || d.handler == nil || d.registrations == nil || ctx == nil {
		return errors.New("invalid unclaimed destroyer")
	}
	h := d.handler
	fresh, err := h.ledger.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		return fmt.Errorf("re-read reservation %s before destroying its guest: %w", vm.ID, err)
	}
	if fresh.ID != vm.ID {
		return fmt.Errorf("%w: ledger returned reservation %s for %s", store.ErrConflict, fresh.ID, vm.ID)
	}
	done, err := unclaimedDestroyable(fresh)
	if err != nil || done {
		return err
	}
	if err := d.confirmRegistrationGone(ctx, fresh); err != nil {
		return err
	}
	if !fresh.CleanupRequested {
		fresh, err = h.ledger.MarkRunnerVMDraining(ctx, h.config.Actor, fresh.ID, fresh.Generation)
		if err != nil {
			return fmt.Errorf("request cleanup for unclaimed reservation %s: %w", vm.ID, err)
		}
	}
	if fresh.State == "draining" {
		proof, err := unclaimedSafety(fresh, time.Now().UTC())
		if err != nil {
			return err
		}
		if fresh, err = h.execute(ctx, fresh, "stop", proof); err != nil {
			return fmt.Errorf("stop unclaimed guest %d: %w", vm.VMID, err)
		}
	}
	if fresh.State != "stopped" || !fresh.CleanupRequested {
		return fmt.Errorf("%w: unclaimed reservation %s is %q and not ready to destroy",
			ErrUnclaimedIncomplete, vm.ID, fresh.State)
	}
	proof, err := d.destroyProof(ctx, fresh)
	if err != nil {
		return err
	}
	if _, err := h.execute(ctx, fresh, "destroy", proof); err != nil {
		return fmt.Errorf("destroy unclaimed guest %d: %w", vm.VMID, err)
	}
	return nil
}

// confirmRegistrationGone asks the forge again rather than trusting that the
// first step of the sequence ran. Revocation is idempotent and answers false
// when there is nothing registered, so the ordinary answer here is "nothing",
// and that is what licenses RunnerDeregistered on the evidence below.
//
// A registration found live between step one and step two is not something to
// destroy through. It has just been removed by this very call, so the row is
// left in the queue and the next pass walks the sequence from the top against
// a forge that now agrees with the ledger.
func (d *UnclaimedDestroyer) confirmRegistrationGone(ctx context.Context, vm store.RunnerVM) error {
	ref, err := runnerRef(vm)
	if err != nil {
		return err
	}
	removed, err := d.registrations.Revoke(ctx, ref)
	if err != nil {
		return fmt.Errorf("confirm registration is gone for reservation %s: %w", vm.ID, err)
	}
	if removed {
		return fmt.Errorf("%w: reservation %s still held a live registration at destroy time",
			ErrUnclaimedIncomplete, vm.ID)
	}
	return nil
}

// unclaimedSafety is the evidence an unclaimed teardown carries. Drained and
// NoActiveJob are the ledger's claimed_at read back as a safety claim, not an
// observation of the guest; the caller has just re-checked that column under
// a fresh read. RunnerDeregistered is only set because confirmRegistrationGone
// asked the forge and it said nothing is registered.
func unclaimedSafety(vm store.RunnerVM, observedAt time.Time) (store.RunnerVMSafetyEvidence, error) {
	if vm.ClaimedAt != nil {
		return store.RunnerVMSafetyEvidence{}, fmt.Errorf("%w: reservation %s is claimed", store.ErrConflict, vm.ID)
	}
	if observedAt.IsZero() {
		return store.RunnerVMSafetyEvidence{}, fmt.Errorf("%w: unclaimed safety evidence needs a clock", store.ErrInvalid)
	}
	evidenceID, err := store.NewID()
	if err != nil {
		return store.RunnerVMSafetyEvidence{}, fmt.Errorf("unclaimed safety evidence ID: %w", err)
	}
	return store.RunnerVMSafetyEvidence{
		EvidenceID:         evidenceID,
		ObservedAt:         observedAt,
		Drained:            true,
		NoActiveJob:        true,
		RunnerDeregistered: true,
		ExternalRunnerID:   vm.ExternalRunnerID,
	}, nil
}

// destroyProof adds what a destroy needs beyond the unclaimed argument: the
// operator's cleanup approval and a digest read off the guest that is about
// to be deleted. The guest is observed here and not earlier on purpose, so
// the digest and the freshness window belong to this attempt.
func (d *UnclaimedDestroyer) destroyProof(ctx context.Context, vm store.RunnerVM) (store.RunnerVMSafetyEvidence, error) {
	h := d.handler
	if !store.ValidID(h.config.CleanupApprovalID) {
		return store.RunnerVMSafetyEvidence{}, errors.New("unclaimed destroy needs a reviewed cleanup approval")
	}
	identity, err := h.identity(vm)
	if err != nil {
		return store.RunnerVMSafetyEvidence{}, err
	}
	observed, err := h.vms.Get(ctx, identity)
	if err != nil {
		return store.RunnerVMSafetyEvidence{}, fmt.Errorf("observe unclaimed guest %d: %w", vm.VMID, err)
	}
	if observed.Status != "stopped" || observed.Protected || observed.Locked || !digestPattern.MatchString(observed.ConfigDigest) {
		return store.RunnerVMSafetyEvidence{}, fmt.Errorf("unclaimed guest %d is not safe for cleanup", vm.VMID)
	}
	proof, err := unclaimedSafety(vm, time.Now().UTC())
	if err != nil {
		return store.RunnerVMSafetyEvidence{}, err
	}
	proof.ApprovalID = h.config.CleanupApprovalID
	proof.ExpectedConfigDigest = observed.ConfigDigest
	return proof, nil
}
