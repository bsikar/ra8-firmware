// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

// A beat comes back in two halves: the board snapshot the server now holds,
// and the liveness report it derived from that snapshot. board.HolderLiveness
// documents Held as "whether a holder exists to have an opinion about", and
// board.ObserveHolderLiveness sets it true for exactly the snapshots that
// carry a lease in a live holder phase. The two halves are therefore one
// answer, and a reply whose snapshot names this holder's lease while its
// liveness half says nobody holds the board is not a report this agent can
// act on.
//
// ReportAlive already refuses the other direction, a liveness half naming a
// lease the token does not, and already refuses a snapshot that no longer
// carries this lease at this generation. This closes the remaining shape: the
// snapshot agrees, the liveness half does not.
//
// What it costs to accept one is quiet rather than loud. An unheld liveness
// report is zero throughout, so the caller reads a holder with no last-seen
// stamp and no next-beat deadline, and KeepAlive adopts a zero Interval,
// which beatInterval floors to the default minute. A holder told to beat
// every two seconds would then beat once a minute and read as overdue to
// everybody watching, on a board it still holds.

// beatHalvesAgree reports whether the two halves of a beat describe the same
// board. It judges only the disagreement the caller has not already excluded,
// so it is asked after the snapshot has been held to the token.
func beatHalvesAgree(snapshot board.Snapshot, liveness boardclient.HolderLiveness) bool {
	if liveness.Held {
		return true
	}
	return snapshot.Lease == nil
}
