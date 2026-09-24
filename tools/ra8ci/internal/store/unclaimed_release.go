// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
)

// The last step of the unclaimed-runner sequence: the write that actually
// takes a reservation out of the reaper's queue. The three steps before it
// live in other systems (the forge, Proxmox, the bench), so each of them can
// fail and be retried without the ledger knowing. This one is the commit
// point, and it is the only step that must never run early: while the row is
// still unreleased the next pass finds it again and walks the sequence from
// the top, which is exactly the recovery a half-finished revocation needs.

// unclaimedRelease reports whether a locked reservation may be released as
// unclaimed, and says no in the caller's own vocabulary rather than a bare
// bool. It takes no clock: the deadline was already the queue's business, and
// a caller holding the row wants the half of the predicate that cannot have
// moved since. Three answers matter.
//
//   - already released: nil with done=true, because a repeated pass finding
//     its own finished work is the normal case, not a conflict.
//   - claimed: ErrConflict. The job took the runner between the queue read
//     and the lock, and releasing now would tear down live work.
//   - unknown outcome: ErrConflict. A Proxmox operation is in flight against
//     this reservation; releasing underneath it would strand the guest the
//     operation is about to report on.
func unclaimedRelease(vm RunnerVM) (done bool, err error) {
	switch {
	case vm.State == "released":
		return true, nil
	case vm.ClaimedAt != nil:
		return false, fmt.Errorf("%w: reservation claimed at %s, not the unclaimed reaper's to release",
			ErrConflict, vm.ClaimedAt.UTC().Format(time.RFC3339Nano))
	case vm.UnknownOutcome:
		return false, fmt.Errorf("%w: reservation has operation %s outstanding",
			ErrConflict, vm.CurrentOperationID)
	default:
		return false, nil
	}
}

// ReleaseUnclaimedRunnerVM ends a reservation whose credential nobody took.
// It is the abandon step of the unclaimed sequence, and it is idempotent: a
// reservation already released comes back unchanged, so a pass resuming after
// an ambiguous failure can run the whole sequence again.
//
// The guard is re-run under the row lock rather than trusted from the queue
// read, because the interesting case for this reaper is precisely the job that
// claims its runner while the batch is being walked.
func (s *Store) ReleaseUnclaimedRunnerVM(ctx context.Context, actor, reservationID string) (RunnerVM, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) {
		return RunnerVM{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: begin unclaimed release: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+`
		FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: lock unclaimed release: %v", ErrUnavailable, err)
	}
	done, err := unclaimedRelease(vm)
	if err != nil {
		return RunnerVM{}, err
	}
	if done {
		return vm, nil
	}
	// ended_at is set in the same statement as the state: the schema holds
	// (state='released') = (ended_at IS NOT NULL), so the two cannot be
	// written apart. The generation bump is what invalidates any operation
	// CAS an in-flight caller is still holding.
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET state='released',generation=generation+1,
		updated_at=clock_timestamp(),ended_at=clock_timestamp()
		WHERE id=$1 AND generation=$2 AND claimed_at IS NULL
			AND unknown_outcome=false AND state<>'released'`, vm.ID, vm.Generation)
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVM{}, fmt.Errorf("%w: unclaimed release CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.unclaimed_released", "runner_vm", vm.ID,
		"ok", vm.State, "released", "", map[string]any{
			"generation":         vm.Generation + 1,
			"unclaimed_deadline": vm.UnclaimedDeadline.UTC().Format(time.RFC3339Nano),
			"external_runner_id": vm.ExternalRunnerID,
			"vmid":               vm.VMID,
		}); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: audit unclaimed release: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: commit unclaimed release: %v", ErrUnavailable, err)
	}
	return s.GetRunnerVM(ctx, vm.ID)
}
