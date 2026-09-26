// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The unclaimed sequence asks four systems about one reservation, and every
// other step in it holds the answer against the question before reading a
// field off it. The re-read at the top of a pass refuses a row naming another
// reservation; the destroy step re-reads and refuses the same way; the commit
// step refuses a released row that names somebody else. The bench step did
// not: it asked for the leases held under one identity and read whatever came
// back.
//
// It is the step where an unheld answer does the most surprising thing,
// because this guard's finding is a refusal. A row from another holder does
// not release anything by mistake; it reports ErrUnclaimedLeaseHeld, which
// says the bench holds live hardware for a reservation nobody claimed. That
// answer stops the sequence, leaves the reservation queued with its
// registration revoked and its guest still there, and counts towards the
// eight failures that abandon the whole pass. It also sends an operator to a
// board that has nothing to do with this reservation, carrying an error whose
// own wording invites them to go and end the lease on it.
//
// So the two findings are told apart. A lease genuinely held under this
// reservation's identity is ErrUnclaimedLeaseHeld, unchanged: a contradiction
// between the ledger and the bench that a person resolves by looking at real
// hardware. A bench answering about a different holder is ErrConflict: the
// read itself cannot be trusted, and nothing about this reservation has been
// learned from it.
//
// What is deliberately NOT restated here is which states count as live. That
// rule is the bench's, written once in store.liveLeaseStates and applied in
// the query, and a second copy in this package would be free to drift from
// the one that actually selects the rows.

// heldLease picks the lease to report from a non-empty bench answer, after
// holding that answer to the holder it was asked about. Identity first: a row
// naming another holder says nothing about this reservation, and a row that
// names neither itself nor a board cannot be acted on by the operator the
// refusal is written for.
func heldLease(asked string, live []store.LiveBoardLease) (store.LiveBoardLease, error) {
	if asked == "" {
		return store.LiveBoardLease{}, fmt.Errorf("%w: bench was asked about no holder", store.ErrInvalid)
	}
	if len(live) == 0 {
		return store.LiveBoardLease{}, fmt.Errorf("%w: no live lease to report for holder %s", store.ErrInvalid, asked)
	}
	for _, lease := range live {
		if lease.HolderID != asked {
			return store.LiveBoardLease{}, fmt.Errorf("%w: bench returned lease %s held by %s for holder %s",
				store.ErrConflict, lease.ID, lease.HolderID, asked)
		}
	}
	reported := live[0]
	if reported.ID == "" || reported.BoardID == "" {
		return store.LiveBoardLease{}, fmt.Errorf("%w: bench reported a live lease for holder %s naming no lease and no board",
			store.ErrConflict, asked)
	}
	return reported, nil
}
