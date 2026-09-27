package board

import (
	"testing"
	"time"
)

// arrivalWaiter is a valid queue entry with only the two facts this door reads
// left to the caller.
func arrivalWaiter(id string, sequence uint64, queuedAt time.Time) Waiter {
	return Waiter{
		ID:       id,
		LeaseID:  "lease-" + id,
		Holder:   "agent-" + id,
		Class:    ClassCI,
		Reason:   "bench debug",
		Duration: 20 * time.Minute,
		Sequence: sequence,
		QueuedAt: queuedAt,
	}
}

// arrivalBoard is a quarantined board carrying the given queue and nothing
// else, so a test moves only the order. Quarantined rather than ready because a
// ready board is held to an empty queue: grantNext serves a free board's queue
// inside the command that fills it, and the quarantined board retains its
// waiters on purpose while nothing can be granted.
func arrivalBoard(queue ...Waiter) Snapshot {
	var highest uint64
	for _, w := range queue {
		if w.Sequence > highest {
			highest = w.Sequence
		}
	}
	return Snapshot{
		BoardID:      "board-a",
		Phase:        Quarantined,
		Version:      3,
		NextSequence: highest,
		Queue:        queue,
	}
}

func arrivalTime(minute int) time.Time {
	return time.Date(2026, 9, 26, 9, minute, 0, 0, time.UTC)
}

func TestAQueueInArrivalOrderIsAdmitted(t *testing.T) {
	queue := []Waiter{
		arrivalWaiter("w1", 1, arrivalTime(1)),
		arrivalWaiter("w2", 2, arrivalTime(2)),
		arrivalWaiter("w3", 3, arrivalTime(3)),
	}
	if err := Validate(arrivalBoard(queue...)); err != nil {
		t.Fatalf("ordered queue refused: %v", err)
	}
}

func TestAQueueOutOfArrivalOrderIsRefused(t *testing.T) {
	queue := []Waiter{
		arrivalWaiter("w1", 1, arrivalTime(1)),
		arrivalWaiter("w3", 3, arrivalTime(3)),
		arrivalWaiter("w2", 2, arrivalTime(2)),
	}
	err := Validate(arrivalBoard(queue...))
	if err == nil {
		t.Fatal("shuffled queue accepted")
	}
	if !IsCode(err, Conflict) {
		t.Fatalf("code = %v, want conflict", err)
	}
}

func TestTheRefusalNamesBothWaitersAndTheirSequences(t *testing.T) {
	queue := []Waiter{
		arrivalWaiter("w1", 1, arrivalTime(1)),
		arrivalWaiter("w9", 9, arrivalTime(9)),
		arrivalWaiter("w4", 4, arrivalTime(4)),
	}
	err := Validate(arrivalBoard(queue...))
	if err == nil {
		t.Fatal("shuffled queue accepted")
	}
	// The pair is the whole finding: an operator reading the refusal needs
	// to know which two rows disagree, not merely that some pair does.
	detail := err.Error()
	for _, want := range []string{`"w4"`, `"w9"`, "4", "9"} {
		if !contains(detail, want) {
			t.Fatalf("detail = %q, want it to name %s", detail, want)
		}
	}
}

func TestTheFirstDisorderedPairIsTheOneReported(t *testing.T) {
	queue := []Waiter{
		arrivalWaiter("w5", 5, arrivalTime(5)),
		arrivalWaiter("w2", 2, arrivalTime(2)),
		arrivalWaiter("w9", 9, arrivalTime(9)),
		arrivalWaiter("w7", 7, arrivalTime(7)),
	}
	err := Validate(arrivalBoard(queue...))
	if err == nil {
		t.Fatal("shuffled queue accepted")
	}
	if !contains(err.Error(), `"w2"`) {
		t.Fatalf("detail = %q, want the first disordered pair", err.Error())
	}
}

func TestAnEmptyOrSingleWaiterQueueIsAdmitted(t *testing.T) {
	if err := Validate(arrivalBoard()); err != nil {
		t.Fatalf("empty queue refused: %v", err)
	}
	if err := Validate(arrivalBoard(arrivalWaiter("w1", 1, arrivalTime(1)))); err != nil {
		t.Fatalf("single waiter refused: %v", err)
	}
}

func TestGapsLeftByDepartedWaitersAreAdmitted(t *testing.T) {
	// cancel and grantNext remove elements without renumbering, so a live
	// queue is routinely 3, 17, 40. Only the direction is the rule.
	queue := []Waiter{
		arrivalWaiter("w3", 3, arrivalTime(3)),
		arrivalWaiter("w17", 17, arrivalTime(17)),
		arrivalWaiter("w40", 40, arrivalTime(40)),
	}
	if err := Validate(arrivalBoard(queue...)); err != nil {
		t.Fatalf("gapped queue refused: %v", err)
	}
}

func TestTheOrderIsJudgedOnSequenceNotQueuedAt(t *testing.T) {
	// Sequence is the queue's own counter; a wall-clock stamp that reads
	// backwards is a different accident and not this door's to refuse.
	queue := []Waiter{
		arrivalWaiter("w1", 1, arrivalTime(30)),
		arrivalWaiter("w2", 2, arrivalTime(5)),
	}
	if err := Validate(arrivalBoard(queue...)); err != nil {
		t.Fatalf("queue refused for its stamps: %v", err)
	}
}

func TestADuplicateSequenceKeepsItsOwnRefusal(t *testing.T) {
	queue := []Waiter{
		arrivalWaiter("w1", 4, arrivalTime(1)),
		arrivalWaiter("w2", 4, arrivalTime(2)),
	}
	err := Validate(arrivalBoard(queue...))
	if err == nil {
		t.Fatal("duplicate sequence accepted")
	}
	if !contains(err.Error(), "duplicate queue sequence") {
		t.Fatalf("detail = %q, want the duplicate refusal", err.Error())
	}
	if contains(err.Error(), "arrival order") {
		t.Fatalf("detail = %q, want the duplicate door to answer first", err.Error())
	}
}

func TestEnqueueBuildsAnOrderedQueue(t *testing.T) {
	s, err := New("board-a")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	for i, class := range []Class{ClassAI, ClassCI, ClassHuman, ClassAI} {
		w := arrivalWaiter(string(rune('a'+i)), 0, time.Time{})
		w.Class = class
		s, _, err = Apply(s, Enqueue{Actor: "operator", Waiter: w}, arrivalTime(i))
		if err != nil {
			t.Fatalf("enqueue %d: %v", i, err)
		}
	}
	if err := Validate(s); err != nil {
		t.Fatalf("reducer produced a queue this door refuses: %v", err)
	}
	if err := checkQueueIsInArrivalOrder(s.Queue); err != nil {
		t.Fatalf("queue after four enqueues: %v", err)
	}
}

func TestGrantingFromTheMiddleLeavesTheQueueOrdered(t *testing.T) {
	s, err := New("board-a")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	// Three waiters queue while the board is ready, so the first is granted
	// at once and the highest class is taken out of the middle next.
	for i, class := range []Class{ClassAI, ClassHuman, ClassCI} {
		w := arrivalWaiter(string(rune('a'+i)), 0, time.Time{})
		w.Class = class
		s, _, err = Apply(s, Enqueue{Actor: "operator", Waiter: w}, arrivalTime(i))
		if err != nil {
			t.Fatalf("enqueue %d: %v", i, err)
		}
	}
	if s.Lease == nil {
		t.Fatal("no grant after three enqueues")
	}
	if err := checkQueueIsInArrivalOrder(s.Queue); err != nil {
		t.Fatalf("queue after a grant: %v", err)
	}
}

func TestCancellingFromTheMiddleLeavesTheQueueOrdered(t *testing.T) {
	s, err := New("board-a")
	if err != nil {
		t.Fatalf("new board: %v", err)
	}
	ids := []string{"a", "b", "c", "d"}
	for i, id := range ids {
		w := arrivalWaiter(id, 0, time.Time{})
		s, _, err = Apply(s, Enqueue{Actor: "operator", Waiter: w}, arrivalTime(i))
		if err != nil {
			t.Fatalf("enqueue %s: %v", id, err)
		}
	}
	s, _, err = Apply(s, CancelWaiter{Actor: "operator", WaiterID: "c"}, arrivalTime(9))
	if err != nil {
		t.Fatalf("cancel: %v", err)
	}
	if err := checkQueueIsInArrivalOrder(s.Queue); err != nil {
		t.Fatalf("queue after a cancel: %v", err)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("validate after a cancel: %v", err)
	}
}

func TestTheDoorIsReachedThroughValidate(t *testing.T) {
	// The check is worth nothing sitting beside Validate rather than inside
	// it: the store's load path is the only caller that matters.
	queue := []Waiter{
		arrivalWaiter("w2", 2, arrivalTime(2)),
		arrivalWaiter("w1", 1, arrivalTime(1)),
	}
	if checkQueueIsInArrivalOrder(queue) == nil {
		t.Fatal("door admitted a reversed queue")
	}
	if Validate(arrivalBoard(queue...)) == nil {
		t.Fatal("Validate admitted a reversed queue")
	}
}
