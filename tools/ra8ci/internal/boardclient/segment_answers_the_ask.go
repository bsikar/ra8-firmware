// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardclient

import "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"

// maxSegmentKeyBytes restates the bound the store applies to a segment key
// before it writes one, so a key this client is willing to carry back is a key
// the server was willing to record.
const maxSegmentKeyBytes = 128

// segmentAnswersTheAsk reports whether the segment the server returned is the
// bounded operation this caller actually began.
//
// Every other read door in this client holds what comes back to a rule: Status
// and command run board.Validate over the snapshot, WaitForGrant compares the
// lease against the ticket it is waiting on, and ClaimNextHILAttempt refuses an
// assignment that is not catalog-bound. BeginSegment was the one door that
// handed a server document straight to its caller, decoded and otherwise
// untouched.
//
// The document matters because ONE field of it is load-bearing beyond this
// call. internal/boardagent takes segment.ID out of the result and closes over
// it, and that closure is the unwind path: the deferred finish runs on a fresh
// five-second context after the fixture has been touched, often while the
// caller is already handling a deadline or a stale generation. FinishSegment
// then names that identifier in the request path and states this lease and
// generation beside it. So an identifier that did not come from this ask is
// carried, unexamined, into the one write the agent makes with no read in
// front of it, and FinishSegment's own door cannot catch it: that door judges
// the TOKEN, which is this caller's and correct, and asks only that the
// segment identifier is non-empty.
//
// What the fields have to say is fixed entirely by what was asked, and the
// store decides all of them inside the same transaction that takes the
// per-board lock: the segment belongs to this board, this lease and this
// generation, it records the attempt and key that were sent, and its deadline
// sits after its start. A server answering anything else is not describing the
// operation that was begun, whether because a proxy served another board's
// document, because a response was matched to the wrong request, or because
// the far end is not the board service at all. The client cannot tell which,
// and does not need to: it knows what it asked for.
//
// A request the client knows the server must refuse is refused here rather
// than sent, the same rule WithCorrelationID states for an identifier the
// server would not echo. This is the reading half of that: an answer the
// client can see is not about its own ask is refused here rather than acted
// on, because the action it would otherwise authorize is a write against
// hardware someone else may hold.
//
// How much of the bound is left by the time the answer is read is NOT judged
// here. That is the agent's question, settled against its own monotonic clock
// around the request, and a wall-clock stamp from another host cannot answer
// it. Only the pair's ORDER is judged: a deadline at or before the start is a
// bound of zero or less, which is the one shape BeginSegment already refused
// on the way out.
func segmentAnswersTheAsk(segment store.BoardSegment, token LeaseToken, attemptID, key string) bool {
	if !store.ValidID(segment.ID) || segment.BoardID != token.BoardID ||
		segment.LeaseID != token.LeaseID || segment.Generation != token.Generation {
		return false
	}
	if segment.AttemptID != attemptID || segment.Key != key || !usableSegmentKey(segment.Key) {
		return false
	}
	if segment.StartedAt.IsZero() || segment.DeadlineAt.IsZero() {
		return false
	}
	return segment.DeadlineAt.After(segment.StartedAt)
}

// usableSegmentKey restates the store's own key rule. A key the store would
// not have written is a key this answer did not come from.
func usableSegmentKey(key string) bool {
	if key == "" || len(key) > maxSegmentKeyBytes {
		return false
	}
	for _, r := range key {
		if r < 0x20 || r == 0x7f {
			return false
		}
	}
	return true
}
