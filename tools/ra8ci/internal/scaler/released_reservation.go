// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The unclaimed sequence asks the ledger about a reservation twice, and both
// answers are rows the pass then acts on. The destroy step re-reads the row
// and refuses one carrying another reservation's ID before it touches a
// hypervisor. The commit step took the row the release returned and read only
// its state.
//
// Those two doors state one rule: a row is an answer about the reservation it
// was asked about, and a row naming a different one is a contradiction, not a
// reservation to judge. The commit step is the worse place to leave it
// unstated, because it is the only step in the sequence that cannot be
// retried from the queue: once a row is released the pass has no handle on
// it, so a reservation closed on another row's state is closed on evidence
// nobody can go back and check.
//
// The state rule is unchanged and its wording is deliberately the one the
// step already used. What is new is that it is now stated about a row this
// pass knows is the right one.

// checkReleasedReservation holds the ledger's answer to the reservation the
// pass asked to close. Identity first: a row naming another reservation says
// nothing about this one, and reading its state would be reading somebody
// else's.
func checkReleasedReservation(asked string, released store.RunnerVM) error {
	if asked == "" {
		return fmt.Errorf("%w: release was asked about no reservation", store.ErrInvalid)
	}
	if released.ID != asked {
		return fmt.Errorf("%w: ledger returned reservation %s for %s",
			store.ErrConflict, released.ID, asked)
	}
	if released.State != "released" {
		return fmt.Errorf("%w: reservation %s is %s after release",
			store.ErrConflict, asked, released.State)
	}
	return nil
}
