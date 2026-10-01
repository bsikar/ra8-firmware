package board

import "fmt"

// checkAReadyBoardKeepsNoWaiters holds a ready board to the one queue it can
// have, which is none.
//
// Ready means the board is free and nobody holds it. The reducer reaches that
// phase in exactly two places, release and completeRecovery, and both hand the
// snapshot straight to grantNext before returning; New starts a board ready
// with no queue at all. grantNext takes the first waiter it finds whenever the
// phase is Ready and no lease is retained, so the only way out of it with a
// waiter still queued is an error, and Apply throws the whole attempt away on
// an error and commits the state it held before. enqueue appends and then
// calls grantNext too, so a waiter arriving at a free board is granted inside
// the same command. A ready board carrying a queue is therefore a state this
// reducer cannot produce.
//
// It is worth refusing rather than ignoring because nothing about it looks
// wrong and nothing fails. Every waiter on it is individually valid, the phase
// is the healthy one, and the board reports itself free. What it means is that
// the named waiters are waiting for a board that is already theirs: no clock is
// running against them, no yield is owed by anybody, and no one is late. The
// wait only ends when some unrelated command happens to arrive and drives
// grantNext, so the queue's first waiter is served at a time decided by other
// people's traffic. A snapshot in that shape came from outside the reducer, a
// hand-edited row or a restore that kept the queue and dropped the lease, and
// the evidence for that is strongest at the moment it is loaded.
//
// Judged after the per-waiter scan and after the arrival-order door, so a queue
// that is also invalid or shuffled keeps the refusal naming the row at fault.
// Only Ready is held to it: the recovery phases and Quarantined retain a queue
// on purpose, since grantNext refuses to grant in them and the waiters are
// meant to still be there when the board comes back.
func checkAReadyBoardKeepsNoWaiters(phase Phase, queue []Waiter) error {
	if phase != Ready || len(queue) == 0 {
		return nil
	}
	return &Error{Conflict, fmt.Sprintf(
		"ready board retains %d queued waiter(s), first %q at sequence %d, which a free board would already have granted",
		len(queue), queue[0].ID, queue[0].Sequence)}
}
