// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"fmt"
	"time"
)

// The bench half of the unclaimed sequence's third step.
//
// Nothing in the schema joins a reservation to a bench lease, and that is not
// an oversight: a lease is taken at StartAttempt (internal/store/attempts.go)
// by an agent that already holds a claimed runner, and board leases hang off
// attempts, which key on task_id, while runner_vms keys on
// (scale_set_id, workflow_run_id, workflow_attempt, job_id). A reservation
// nobody claimed therefore never reached the code that acquires one, so the
// expected answer for the step is "this reservation holds no lease".
//
// Expected is not the same as known. The reaper is walked exactly when the
// plane's picture of a reservation has already gone wrong once, so the step
// asks the bench by the only identities the plane could have written into a
// holder id and requires the bench to agree. What it must not do is guess: a
// live lease on a board is somebody's hardware, and ending one on a name
// match with no ledger link is how a reaper takes a board away from work
// that is running fine.

// LiveBoardLease is a pending or active grant, read-only. It carries what an
// operator needs to find the lease and decide, not the whole row: the reaper
// is not authorized to end it.
type LiveBoardLease struct {
	ID         string
	BoardID    string
	HolderID   string
	Priority   string
	State      string
	Generation int64
	GrantedAt  time.Time
	ExpiresAt  time.Time
}

// liveLeaseStates is the bench's own definition of a lease that still holds a
// board, written once. board_one_live_lease_idx (migration 0001) is unique on
// board_id over exactly these two states, so this is the set the schema
// itself treats as live rather than a second opinion about it.
const liveLeaseStates = `state IN ('pending','active')`

// UnclaimedLeaseHolders is the set of holder ids this reservation could
// appear under on the bench, derived from the ledger row alone and with no
// clock in it. The reservation id is what the plane calls the reservation
// everywhere else; the external runner name is what the forge and the runner
// itself use, and it is the identity a holder id would most plausibly be
// built from. Both are asked about because either being present is a
// contradiction worth stopping for, and neither is invented: an empty name is
// dropped rather than turned into a wildcard.
func UnclaimedLeaseHolders(vm RunnerVM) []string {
	holders := make([]string, 0, 2)
	seen := map[string]struct{}{}
	for _, candidate := range []string{vm.ID, vm.ExternalRunnerName} {
		if candidate == "" {
			continue
		}
		if _, dup := seen[candidate]; dup {
			continue
		}
		seen[candidate] = struct{}{}
		holders = append(holders, candidate)
	}
	return holders
}

// ListLiveBoardLeasesByHolder returns every pending or active lease held
// under one holder id. It is a read: the reaper uses it to prove the bench has
// nothing of this reservation's, and an operator uses it to see what the
// reaper refused to walk past. Ordered so a repeated call reads the same.
func (s *Store) ListLiveBoardLeasesByHolder(ctx context.Context, holderID string) ([]LiveBoardLease, error) {
	if s == nil || s.pool == nil || ctx == nil || holderID == "" || len(holderID) > 512 {
		return nil, fmt.Errorf("%w: live lease lookup needs a holder", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT id::text,board_id,holder_id,priority,state,generation,granted_at,expires_at
		FROM board_leases WHERE holder_id=$1 AND `+liveLeaseStates+`
		ORDER BY granted_at,id`, holderID)
	if err != nil {
		return nil, fmt.Errorf("%w: list live leases for %s: %v", ErrUnavailable, holderID, err)
	}
	defer rows.Close()
	var result []LiveBoardLease
	for rows.Next() {
		var lease LiveBoardLease
		if err := rows.Scan(&lease.ID, &lease.BoardID, &lease.HolderID, &lease.Priority,
			&lease.State, &lease.Generation, &lease.GrantedAt, &lease.ExpiresAt); err != nil {
			return nil, fmt.Errorf("%w: live lease scan: %v", ErrUnavailable, err)
		}
		result = append(result, lease)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: live lease rows: %v", ErrUnavailable, err)
	}
	return result, nil
}
