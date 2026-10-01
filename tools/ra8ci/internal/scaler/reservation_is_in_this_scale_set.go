// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A reaper pass is bound to one scale set. The handler builds it from its own
// configuration so the pass, the credentials it reaps and the actor it reaps
// them as cannot come from different deployments, and the queue read carries
// that scale set into the ledger: ListExpiredUnclaimedRunnerVMs selects on
// scale_set_id=$1 before anything else.
//
// The pass then asks the ledger a second question, and that one does not
// carry it. GetRunnerVM reads by id alone, `WHERE id=$1`, with no scale set
// in the predicate, and the re-read row is what every following step acts on:
// the registration revoked at the forge is the row's ExternalRunnerID, the
// guest destroyed on the hypervisor is the row's VMID on the row's node, and
// the reservation closed at the end is the row itself. Nothing downstream
// holds a scale set of its own that a foreign row would fail against. The
// revoker is wired with one forge client, one hypervisor and one actor and
// applies them to whatever row it is handed.
//
// So this is the door for the other half of the question the queue asked.
// The identity half is already stated twice, in the re-read that refuses a
// row naming another reservation and in the commit step that refuses a
// released row naming somebody else; both say the same thing, that a row is
// an answer about the thing it was asked about. A row from another scale set
// is the same contradiction one level up, and it is the more expensive one to
// walk past: an id mismatch is caught before any system is touched, while a
// row that is plausibly this reservation and belongs to another deployment
// gets its registration revoked and its guest destroyed under this pass's
// actor, and the destroy is the step in the sequence nobody can undo.
//
// Both ledger reads are held, not just the re-read. The queue is the read
// whose whole job is to apply the scale set, so a row in its answer carrying
// a different one means the predicate did not do what the pass believes it
// does, and every other row in that batch was selected by the same predicate.
// The pass stops there rather than reaping the rest of an answer it has
// stopped being able to read.

// checkReservationScaleSet holds a ledger answer to the scale set this pass is
// reaping. source names which of the two reads produced the row, because the
// two failures mean different things: a queue row from elsewhere says the
// candidate set cannot be trusted, and a re-read row from elsewhere says the
// row moved between the two reads or the id names something this pass was
// never asked about.
func checkReservationScaleSet(asked int64, source string, vm store.RunnerVM) error {
	if asked <= 0 {
		return fmt.Errorf("%w: pass was asked about no scale set", store.ErrInvalid)
	}
	if source == "" {
		return fmt.Errorf("%w: reservation %s came from no named read", store.ErrInvalid, vm.ID)
	}
	if vm.ScaleSetID != asked {
		return fmt.Errorf("%w: %s returned reservation %s in scale set %d, reaping scale set %d",
			store.ErrConflict, source, vm.ID, vm.ScaleSetID, asked)
	}
	return nil
}

// The two reads, named where the refusal is written rather than spelled at
// each call site.
const (
	unclaimedQueueRead  = "expired queue"
	unclaimedRereadRead = "re-read"
)
