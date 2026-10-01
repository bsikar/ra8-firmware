// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import (
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// extensionIsLater reports whether asking for expiry actually extends the
// lease this snapshot records.
//
// An extension is a request for MORE deadline. The reducer says so outright:
// board.extend refuses anything that is not strictly after the lease's current
// expiry, because a lease's deadline is the one number a holder's safety
// argument rests on and nothing in the plane may quietly move it earlier. This
// client asked only that the time was non-zero, so a request the server must
// refuse was sent anyway.
//
// The comparison is against the SNAPSHOT's expiry, not the token's. A token
// carries the expiry that was in force when the grant was read, and Extend
// itself moves that number without handing the caller a new token, so a holder
// that extends twice is holding a stale figure by its second call. The
// snapshot in the Extend loop was read one step earlier from the server that
// owns the deadline, and it is what the reducer will compare against.
//
// A request the client knows the server must refuse is refused here rather
// than sent, the same rule WithCorrelationID states for an identifier the
// server would not echo. It matters most where the two ends disagree about
// what happened: a holder whose lease is ALREADY later than the time it asked
// for is not failing, it is asking for a deadline it has, and a transport-shaped
// refusal arriving mid-run reads to a caller as having lost its extension.
func extensionIsLater(snapshot board.Snapshot, expiry time.Time) bool {
	if snapshot.Lease == nil || expiry.IsZero() {
		return false
	}
	return expiry.After(snapshot.Lease.ExpiresAt)
}
