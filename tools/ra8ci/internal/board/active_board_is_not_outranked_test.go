package board

import (
	"testing"
	"time"
)

// An active board claims one thing about the queue behind it: nobody queued
// outranks the holder without the holder having been asked to yield. The
// reducer raises that ask itself, and these hold it to that on every path that
// can create or clear the pairing.
//
// It is pinned here rather than in Validate on purpose. The pairing is a state
// the reducer does not produce, but it is one a caller can still hand to
// Validate, and RequestYield's own Active branch is written to take exactly
// such a board and move it to YieldRequested; a validator refusing it would
// refuse that command's stated input and the fixtures across this package that
// build a board by hand rather than by replaying commands. So the guarantee
// belongs to the transitions, and this is where it is held.
//
// What it protects: the ask is raised on the transition, once, and no later
// command re-examines a board that is already Active. Drop the higherWaiting
// call in acknowledge and a lab waiting on a human-class request sits behind an
// agent lease until that lease expires on its own clock, with no yield event
// ever filed and nothing in the history to say why the wait was long.
// CanStartSegment reads the same fact the opposite way, refusing a new segment
// while a higher waiter is queued, so the holder is stopped from working while
// never being asked to hand back: the board goes quiet at both ends at once.

func outrankedWaiter(id string, class Class) Waiter {
	return Waiter{
		ID:       id,
		LeaseID:  "lease-" + id,
		Holder:   "holder-" + id,
		Class:    class,
		Reason:   "bench debug",
		Duration: 15 * time.Minute,
	}
}

func outrankedGrantedAt() time.Time { return time.Date(2026, 9, 26, 9, 0, 0, 0, time.UTC) }

// outrankedBoard is a board in the given phase holding a lease of holderClass,
// with the given waiters queued behind it in arrival order.
func outrankedBoard(phase Phase, holderClass Class, queued ...Waiter) Snapshot {
	granted := outrankedGrantedAt()
	lease := &Lease{
		ID:                "lease-held",
		WaiterID:          "w-held",
		Holder:            "holder-held",
		Class:             holderClass,
		Reason:            "nightly run",
		Generation:        4,
		GrantedAt:         granted,
		ExpiresAt:         granted.Add(20 * time.Minute),
		RequestedDuration: 20 * time.Minute,
		DeadlineVersion:   1,
	}
	s := Snapshot{
		BoardID:        "board-a",
		Phase:          phase,
		Generation:     lease.Generation,
		AgentHighWater: lease.Generation,
		Version:        7,
		Lease:          lease,
	}
	if phase == GrantPending {
		s.AgentHighWater = lease.Generation - 1
	}
	if phase == YieldRequested || phase == Draining {
		lease.YieldRequestedAt = granted.Add(time.Minute)
	}
	for i, w := range queued {
		w.Sequence = uint64(i + 1)
		w.QueuedAt = granted.Add(time.Duration(i+1) * time.Minute)
		s.Queue = append(s.Queue, w)
		s.NextSequence = w.Sequence
	}
	return s
}

// activeIsNotOutranked is the invariant itself, asked of a snapshot the reducer
// has just produced.
func activeIsNotOutranked(t *testing.T, s Snapshot, step string) {
	t.Helper()
	if err := Validate(s); err != nil {
		t.Fatalf("%s produced an invalid snapshot: %v", step, err)
	}
	if s.Phase == Active && higherWaiting(s) {
		t.Fatalf("%s left an active board a queued waiter outranks", step)
	}
}

func TestEnqueueingAnOutrankingWaiterAsksForTheYield(t *testing.T) {
	s := outrankedBoard(Active, ClassAI)
	after, _, err := Apply(s, Enqueue{Actor: "operator", Waiter: outrankedWaiter("w1", ClassHuman)}, outrankedGrantedAt().Add(2*time.Minute))
	if err != nil {
		t.Fatalf("enqueue refused: %v", err)
	}
	if after.Phase != YieldRequested {
		t.Fatalf("phase after enqueue = %q, want %q", after.Phase, YieldRequested)
	}
	if after.Lease.YieldRequestedAt.IsZero() {
		t.Fatal("yield was raised without a request time")
	}
	activeIsNotOutranked(t, after, "enqueue")
}

func TestEnqueueingAWaiterThatDoesNotOutrankLeavesTheBoardActive(t *testing.T) {
	s := outrankedBoard(Active, ClassHuman)
	after, _, err := Apply(s, Enqueue{Actor: "operator", Waiter: outrankedWaiter("w1", ClassCI)}, outrankedGrantedAt().Add(2*time.Minute))
	if err != nil {
		t.Fatalf("enqueue refused: %v", err)
	}
	if after.Phase != Active {
		t.Fatalf("phase after enqueue = %q, want %q", after.Phase, Active)
	}
	activeIsNotOutranked(t, after, "enqueue")
}

func TestAcknowledgingAGrantAnOutrankingWaiterBeatsAsksForTheYield(t *testing.T) {
	// enqueue raises the ask only against an Active board, so a human-class
	// request arriving while a grant is still being installed waits for
	// acknowledge to ask. This is the path that would go silent.
	s := outrankedBoard(GrantPending, ClassAI, outrankedWaiter("w1", ClassHuman))
	after, _, err := Apply(s, AcknowledgeGrant{
		Actor:               "board-agent",
		LeaseID:             s.Lease.ID,
		Generation:          s.Lease.Generation,
		InstalledGeneration: s.Lease.Generation,
	}, outrankedGrantedAt().Add(time.Minute))
	if err != nil {
		t.Fatalf("acknowledge refused: %v", err)
	}
	if after.Phase != YieldRequested {
		t.Fatalf("phase after acknowledge = %q, want %q", after.Phase, YieldRequested)
	}
	if after.Lease.YieldRequestedAt.IsZero() {
		t.Fatal("yield was raised without a request time")
	}
	activeIsNotOutranked(t, after, "acknowledge")
}

func TestAcknowledgingAGrantNoWaiterOutranksLandsActive(t *testing.T) {
	s := outrankedBoard(GrantPending, ClassHuman, outrankedWaiter("w1", ClassCI))
	after, _, err := Apply(s, AcknowledgeGrant{
		Actor:               "board-agent",
		LeaseID:             s.Lease.ID,
		Generation:          s.Lease.Generation,
		InstalledGeneration: s.Lease.Generation,
	}, outrankedGrantedAt().Add(time.Minute))
	if err != nil {
		t.Fatalf("acknowledge refused: %v", err)
	}
	if after.Phase != Active {
		t.Fatalf("phase after acknowledge = %q, want %q", after.Phase, Active)
	}
	activeIsNotOutranked(t, after, "acknowledge")
}

func TestCancellingTheLastOutrankingWaiterReturnsToActive(t *testing.T) {
	s := outrankedBoard(YieldRequested, ClassAI,
		outrankedWaiter("w1", ClassHuman),
		outrankedWaiter("w2", ClassAI),
	)
	after, _, err := Apply(s, CancelWaiter{Actor: "operator", WaiterID: "w1"}, outrankedGrantedAt().Add(3*time.Minute))
	if err != nil {
		t.Fatalf("cancel refused: %v", err)
	}
	if after.Phase != Active {
		t.Fatalf("phase after cancel = %q, want %q", after.Phase, Active)
	}
	if !after.Lease.YieldRequestedAt.IsZero() {
		t.Fatal("cleared yield kept its request time")
	}
	activeIsNotOutranked(t, after, "cancel")
}

func TestCancellingOneOfTwoOutrankingWaitersKeepsTheAsk(t *testing.T) {
	s := outrankedBoard(YieldRequested, ClassAI,
		outrankedWaiter("w1", ClassHuman),
		outrankedWaiter("w2", ClassHuman),
	)
	after, _, err := Apply(s, CancelWaiter{Actor: "operator", WaiterID: "w1"}, outrankedGrantedAt().Add(3*time.Minute))
	if err != nil {
		t.Fatalf("cancel refused: %v", err)
	}
	if after.Phase != YieldRequested {
		t.Fatalf("phase after cancel = %q, want %q", after.Phase, YieldRequested)
	}
	activeIsNotOutranked(t, after, "cancel")
}

func TestGrantingLeavesNoHigherWaiterBehind(t *testing.T) {
	// grantNext picks the highest class in the queue, so the board it
	// creates cannot be outranked by what stays queued. The grant is
	// acknowledged here because the pairing is only claimed of Active.
	s := Snapshot{BoardID: "board-a", Phase: Ready, Version: 2}
	now := outrankedGrantedAt()
	for i, w := range []Waiter{
		outrankedWaiter("w-ai", ClassAI),
		outrankedWaiter("w-human", ClassHuman),
		outrankedWaiter("w-ci", ClassCI),
	} {
		var err error
		s, _, err = Apply(s, Enqueue{Actor: "operator", Waiter: w}, now.Add(time.Duration(i)*time.Minute))
		if err != nil {
			t.Fatalf("enqueue %s refused: %v", w.ID, err)
		}
	}
	if s.Phase != GrantPending || s.Lease.WaiterID != "w-ai" {
		t.Fatalf("first grant went to %v in phase %q", s.Lease, s.Phase)
	}
	after, _, err := Apply(s, AcknowledgeGrant{
		Actor:               "board-agent",
		LeaseID:             s.Lease.ID,
		Generation:          s.Lease.Generation,
		InstalledGeneration: s.Lease.Generation,
	}, now.Add(4*time.Minute))
	if err != nil {
		t.Fatalf("acknowledge refused: %v", err)
	}
	// The AI grant was already pending when the human request arrived, so
	// the board is asked to yield the moment the agent installs it.
	if after.Phase != YieldRequested {
		t.Fatalf("phase = %q, want %q", after.Phase, YieldRequested)
	}
	activeIsNotOutranked(t, after, "grant and acknowledge")
}

func TestAnOutrankedHolderCannotStartAnotherSegment(t *testing.T) {
	// The other end of the same fact: while a higher waiter is queued the
	// holder is refused a new segment, which is why leaving the board
	// Active and unasked would take it quiet at both ends.
	s := outrankedBoard(Active, ClassAI, outrankedWaiter("w1", ClassHuman))
	token := Token{BoardID: s.BoardID, LeaseID: s.Lease.ID, Generation: s.Lease.Generation}
	err := CanStartSegment(s, token, outrankedGrantedAt().Add(time.Minute), time.Minute, time.Minute)
	if !IsCode(err, Denied) {
		t.Fatalf("segment start error = %v, want denied", err)
	}
}
