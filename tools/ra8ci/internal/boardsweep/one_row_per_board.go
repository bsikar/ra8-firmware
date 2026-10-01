// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardsweep

import (
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// oneRowPerBoard reduces a swept page to one row per board and says how many
// rows it set aside.
//
// A board can legitimately carry two live lease rows at once. The projection
// writes a lease row in state 'pending' while the board is in GrantPending
// and 'active' once the grant is taken (store/board.go, the leaseState arm of
// the lease projection), and both states are live, so a queued waiter's row
// and the current holder's row sit in board_leases together. The sweep's read
// asks for exactly those two states past their deadline
// (store.ExpiredBoardLeases: state IN ('pending','active') AND expires_at <=
// $1), joined to the single board_snapshots row, so one stuck board comes
// back as two page rows carrying the same BoardID and the same Version.
//
// Ticking both is worse than wasteful. The first tick expires the lease and
// moves the snapshot version; the second applies under the version the page
// was read at, which is now stale, so it earns a conflict the pass counts as
// Overtaken. The operator then reads "found 2 expired lease(s), reclaimed 1,
// already reclaimed 1" for what was one board and one reclamation, and the
// count they use to ask how many boards are stuck overnight is the count that
// is wrong. A board lock is also held for the second round trip, behind every
// board still waiting in the page.
//
// The kept row is the first one for its board, and the read orders by
// expires_at then board_id, so that is the lease whose deadline passed
// longest ago. Which row is kept does not change what the tick does (both
// name the same board at the same version and the reducer re-derives the
// expiry itself), only which deadline the pass is reporting against.
//
// Setting a duplicate aside is not a defect being swallowed: nothing is
// dropped from the report, the count comes back to the caller, and the next
// pass fifteen seconds later reads the ledger fresh.
func oneRowPerBoard(page []store.ExpiredBoardLease) ([]store.ExpiredBoardLease, int) {
	if len(page) < 2 {
		return page, 0
	}
	seen := make(map[string]struct{}, len(page))
	kept := make([]store.ExpiredBoardLease, 0, len(page))
	duplicates := 0
	for _, lease := range page {
		if _, already := seen[lease.BoardID]; already {
			duplicates++
			continue
		}
		seen[lease.BoardID] = struct{}{}
		kept = append(kept, lease)
	}
	return kept, duplicates
}
