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

// The unclaimed-runner reaper. A just-in-time credential is minted before the
// guest that consumes it exists, and it is single use: if the job never takes
// it, four things have to be undone, and the order between them is the whole
// safety argument rather than a preference.
//
// Revocation comes first. While the registration is live the runner can pick
// up a queued job at any moment, and a guest destroyed underneath a job it
// just accepted takes that job down with it. Everything after revocation is
// cleanup that cannot be stolen from: the guest, then the bench lease it may
// be holding, then the attempt row that still says work is outstanding.
//
// This file owns the order and the accounting. Each step's actual write is a
// seam, because the four of them live in three different systems (the forge,
// Proxmox and the ledger) and the reaper has no business knowing how any of
// them commits.

// Reaper steps, in the order they must happen.
const (
	StepRevokeRegistration = "revoke_registration"
	StepDestroyGuest       = "destroy_guest"
	StepReleaseLease       = "release_lease"
	StepAbandonAttempt     = "abandon_attempt"
)

// UnclaimedSteps is the sequence, stated once. A caller reporting on the
// reaper reads the order from here rather than restating it.
func UnclaimedSteps() []string {
	return []string{StepRevokeRegistration, StepDestroyGuest, StepReleaseLease, StepAbandonAttempt}
}

// maxUnclaimedFailures abandons a pass whose every reservation is failing.
// One reservation failing costs that reservation; a forge or hypervisor
// refusing everything is not something to grind through a batch of.
const maxUnclaimedFailures = 8

// ErrUnclaimedIncomplete is a reservation whose revocation stopped partway.
// It is not a lost reservation: the next pass finds it in the same queue,
// because nothing marks it done until every step has.
var ErrUnclaimedIncomplete = errors.New("unclaimed revocation incomplete")

// UnclaimedRevoker performs the four undo steps. Every method must be
// idempotent and safe to call again after an ambiguous failure: the reaper
// resumes a half-finished reservation by running the sequence from the top,
// so a step that already happened is asked to happen again.
type UnclaimedRevoker interface {
	// RevokeRegistration removes the runner registration the credential
	// was minted for, so nothing can accept a job on it. A reservation
	// that never got as far as registering is a no-op, not an error.
	RevokeRegistration(context.Context, store.RunnerVM) error
	// DestroyGuest destroys the guest, if one was ever cloned.
	DestroyGuest(context.Context, store.RunnerVM) error
	// ReleaseLease releases any bench lease the reservation holds.
	ReleaseLease(context.Context, store.RunnerVM) error
	// AbandonAttempt marks the attempt abandoned and closes the
	// reservation, which is what takes the row out of the queue.
	AbandonAttempt(context.Context, store.RunnerVM) error
}

// ExpiredUnclaimedLister is the reaper's work queue. *store.Store satisfies it.
type ExpiredUnclaimedLister interface {
	ListExpiredUnclaimedRunnerVMs(context.Context, int64, time.Time, int) ([]store.RunnerVM, error)
}

// UnclaimedReaperConfig is what a pass needs beyond its two collaborators.
type UnclaimedReaperConfig struct {
	ScaleSetID int64
	BatchSize  int
	// Now is injectable so a test does not have to wait out a deadline.
	Now func() time.Time
}

func (c UnclaimedReaperConfig) now() time.Time {
	if c.Now == nil {
		return time.Now().UTC()
	}
	return c.Now().UTC()
}

func (c UnclaimedReaperConfig) batch() int {
	if c.BatchSize < 1 {
		return 50
	}
	return c.BatchSize
}

// UnclaimedReaper revokes the credentials nobody took.
type UnclaimedReaper struct {
	config  UnclaimedReaperConfig
	queue   ExpiredUnclaimedLister
	revoker UnclaimedRevoker
}

// NewUnclaimedReaper refuses a reaper it could not run rather than failing at
// the first expired reservation.
func NewUnclaimedReaper(config UnclaimedReaperConfig, queue ExpiredUnclaimedLister, revoker UnclaimedRevoker) (*UnclaimedReaper, error) {
	if config.ScaleSetID <= 0 {
		return nil, errors.New("unclaimed reaper needs a scale set")
	}
	if queue == nil || revoker == nil {
		return nil, errors.New("unclaimed reaper needs a queue and a revoker")
	}
	if config.BatchSize < 0 || config.BatchSize > 1000 {
		return nil, fmt.Errorf("unclaimed reaper batch %d outside [0,1000]", config.BatchSize)
	}
	return &UnclaimedReaper{config: config, queue: queue, revoker: revoker}, nil
}

// UnclaimedReport is what one pass did. Reaped counts reservations that
// finished every step; Partial counts ones that stopped midway and will be
// found again next pass.
type UnclaimedReport struct {
	Scanned int
	Reaped  int
	Claimed int
	Partial int
	// Steps counts completed steps by name, whether or not the
	// reservation they belong to finished the sequence. A pass that
	// keeps stopping at the same place says so here: the steps before
	// the failure are counted and the one that failed is not.
	Steps map[string]int
}

func (r *UnclaimedReport) step(name string) {
	if r.Steps == nil {
		r.Steps = map[string]int{}
	}
	r.Steps[name]++
}

// Reap runs one pass. A reservation claimed between the queue read and its
// turn is left alone: the job took the runner while the reaper was walking
// the batch, and revoking then would kill live work. That re-check is the
// reason UnclaimedReservation exists separately from UnclaimedExpired.
//
// The returned error reports that the pass was incomplete, never that the
// report is meaningless: the counts are what actually happened either way.
func (r *UnclaimedReaper) Reap(ctx context.Context) (UnclaimedReport, error) {
	var report UnclaimedReport
	if r == nil || r.queue == nil || r.revoker == nil || ctx == nil {
		return report, errors.New("invalid unclaimed reaper")
	}
	now := r.config.now()
	expired, err := r.queue.ListExpiredUnclaimedRunnerVMs(ctx, r.config.ScaleSetID, now, r.config.batch())
	if err != nil {
		return report, fmt.Errorf("list expired unclaimed reservations: %w", err)
	}
	var failures int
	var firstErr error
	for _, vm := range expired {
		report.Scanned++
		if !store.UnclaimedReservation(vm) {
			report.Claimed++
			continue
		}
		if err := r.revoke(ctx, vm, &report); err != nil {
			report.Partial++
			failures++
			if firstErr == nil {
				firstErr = err
			}
			if failures >= maxUnclaimedFailures {
				return report, fmt.Errorf("%w: abandoned the pass after %d failures: %w",
					ErrUnclaimedIncomplete, failures, firstErr)
			}
			continue
		}
		report.Reaped++
	}
	if firstErr != nil {
		return report, fmt.Errorf("%w: %d of %d reservations: %w",
			ErrUnclaimedIncomplete, report.Partial, report.Scanned, firstErr)
	}
	return report, nil
}

// revoke walks the sequence for one reservation and stops at the first step
// that fails. Nothing later runs: the order exists precisely so a guest is
// never destroyed while its registration is still live, and carrying on past
// a failed revocation would throw that away for the sake of a tidier count.
//
// Each step is counted as it completes rather than four at a time once the
// whole sequence has. A reservation that stops midway has still had real work
// done on the systems behind the steps that ran, and the counts are where an
// operator reads how far a failing pass is getting: without this, a pass
// stopping at destroy_guest every time and a pass stopping at the very first
// step report exactly the same thing, an empty map and a count of failures.
func (r *UnclaimedReaper) revoke(ctx context.Context, vm store.RunnerVM, report *UnclaimedReport) error {
	steps := map[string]func(context.Context, store.RunnerVM) error{
		StepRevokeRegistration: r.revoker.RevokeRegistration,
		StepDestroyGuest:       r.revoker.DestroyGuest,
		StepReleaseLease:       r.revoker.ReleaseLease,
		StepAbandonAttempt:     r.revoker.AbandonAttempt,
	}
	for _, name := range UnclaimedSteps() {
		step, exists := steps[name]
		if !exists || step == nil {
			return fmt.Errorf("unclaimed reaper has no %s step", name)
		}
		if err := step(ctx, vm); err != nil {
			return fmt.Errorf("reservation %s stopped at %s: %w", vm.ID, name, err)
		}
		report.step(name)
	}
	return nil
}
