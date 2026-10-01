// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"

// beatAnswersThisLease reports whether what came back from a heartbeat is an
// answer about the lease the beat was made under.
//
// A beat is the one command in this client that returns two descriptions of
// the same board: the snapshot the reducer produced and the liveness report
// derived from it. Half of that pair was already held to the token. The
// liveness half is refused when it names another lease, on the stated grounds
// that a report about some other lease is not an answer about this one,
// however healthy it looks. The snapshot half was held only to board.Validate
// and the board identifier, which is the weaker check of the two: a snapshot
// that passes both can still record a different lease entirely, and it is the
// snapshot, not the report, that the caller carries away.
//
// What the answer must say is fixed by what the server did. The reducer runs
// current() over the lease and generation named in the request before it will
// record anything, and refuses the beat outright when the board is not in a
// live holder phase, so a 200 is the server stating that this exact lease,
// on this exact generation, still holds this board. ObserveHolderLiveness
// then reads that same committed snapshot, which is why the report must be
// Held, must name the same lease, and must state the lease's own expiry. A
// beat records a heartbeat stamp and nothing else: it cannot move the lease's
// identity, its generation, or its deadline, which is what Heartbeat's own
// documentation promises a caller in both directions.
//
// So an answer that fails this is not the board disagreeing with the holder.
// It is a document that did not come from this ask: another board's response
// matched to this request by a proxy, a reply paired with the wrong caller,
// or a far end that is not the board service at all. The client cannot tell
// which and does not need to, the same judgement segmentAnswersTheAsk makes
// about a begun segment.
//
// It matters because of which numbers a holder acts on. The deadline is the
// one figure a holder's safety argument rests on, and extensionIsLater exists
// because a holder that extends against a stale one is asking for a deadline
// it does not have. A holder beating on its interval, which is the loop that
// runs for the whole life of a lease, would take a foreign expiry straight
// from this call and never read its own again.
func beatAnswersThisLease(snapshot board.Snapshot, reported HolderLiveness, token LeaseToken) bool {
	// sameLease judges the grant alone, deliberately: it is asked about a
	// snapshot a caller just read for a named board. Here the board is
	// part of what is in doubt, so it is named too.
	if snapshot.BoardID != token.BoardID || !sameLease(snapshot, token) {
		return false
	}
	if !reported.Held || reported.LeaseID != token.LeaseID {
		return false
	}
	// Both halves are the server's reading of one committed snapshot, so
	// the deadline it reports is the deadline it recorded.
	return reported.ExpiresAt.Equal(snapshot.Lease.ExpiresAt)
}
