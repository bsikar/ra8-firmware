package board

import "fmt"

// checkQueueIsInArrivalOrder holds a retained wait queue's slice order to the
// sequences carried on its own waiters.
//
// The queue is written in one direction only. enqueue takes the next sequence
// (NextSequence increments, never repeats, never rolls back) and appends the
// waiter at the tail; the only other writers remove an element, cancel taking
// the one it cancels and grantNext taking the one it grants. Nothing in the
// reducer sorts, swaps, or reinserts. So a queue this reducer produced is
// strictly ascending by Sequence in slice order, always, with gaps wherever a
// waiter left.
//
// Who gets the board does not depend on it: grantNext scans the whole queue and
// picks by class, then by the lowest sequence inside that class, so a shuffled
// slice still grants the right waiter. What the slice order decides is what a
// reader is told. The server's yield path and the client's view of who is
// waiting both walk the queue in slice order, so a reordered queue shows a
// waiting list whose head is not the waiter the board will actually grant next,
// and for two waiters of the same class the slice order is the only record of
// arrival a person can see; the sequences themselves are opaque numbers nobody
// reads off a row by eye. The heartbeat write is stricter still: it proves a
// beat changed nothing else by comparing the queues element by element, so a
// queue that came back in another order reads to it as a different queue and a
// legitimate beat is refused for a reason that is nowhere in the beat.
//
// Ordering is judged after Validate's duplicate scan, so a repeated sequence
// keeps its own refusal and cannot reach here; by this point equal sequences
// are already gone. Sequence is judged, not QueuedAt: the reducer stamps both
// from the same Apply clock, but only the sequence is the queue's own counter,
// and a clock that stepped is not the accident this door is about.
func checkQueueIsInArrivalOrder(queue []Waiter) error {
	for i := 1; i < len(queue); i++ {
		if queue[i].Sequence < queue[i-1].Sequence {
			return &Error{Conflict, fmt.Sprintf(
				"retained wait queue is out of arrival order: waiter %q at sequence %d follows %q at sequence %d",
				queue[i].ID, queue[i].Sequence, queue[i-1].ID, queue[i-1].Sequence)}
		}
	}
	return nil
}
