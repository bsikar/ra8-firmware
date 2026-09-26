// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// validLeaseToken reports whether a token states every field the server needs
// to decide whether this caller still holds the board: the board it is about,
// the waiter the grant was made to, the lease the grant created, and the
// generation that lease is on.
//
// It is one rule because a lease-bound request is one claim. Every method that
// reads before it writes reaches the rule through leaseStatus, so a caller
// holding a half-filled token is told so by this client rather than by the far
// end. A method that writes WITHOUT reading first has no such door, and the
// terminal half of a bounded hardware operation is exactly that method: it
// runs on the unwind path, after the fixture has been touched, where the
// caller is often already handling a failure and the token it is unwinding
// with may be whatever survived that failure.
//
// A request the client knows the server must refuse is refused here rather
// than sent, the same rule WithCorrelationID states for an identifier the
// server would not echo. Sent instead, the refusal comes back as a transport
// answer about a segment that did record something, which is the one outcome
// this client cannot tell a caller anything useful about.
func validLeaseToken(token LeaseToken) bool {
	return validBoardID(token.BoardID) && store.ValidID(token.LeaseID) &&
		store.ValidID(token.RequestID) && token.Generation != 0
}
