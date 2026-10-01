package board

// Who outranks whom, and the one place a class outranks itself.
//
// Three small functions decide every priority question this reducer asks, and
// between them they carry a rule nothing states in one place: classCeiling
// (how long a class may hold a board), higherWaiting (whether the holder is
// outranked, which raises a yield ask) and sameClassHumanWaiting (whether a
// human is queued behind a human, which does NOT). The asymmetry in that last
// pair is the whole design and it was unheld: a CI job behind a CI job waits
// quietly, a person behind a person is contention, and neither reading is
// obvious from either call site.

import (
	"testing"
	"time"
)

func rankAt(d time.Duration) time.Time { return testEpoch.Add(d) }

// rankHeld is an Active board held by class, no queue.
func rankHeld(t *testing.T, class Class) Snapshot {
	t.Helper()
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("holder", class), testEpoch)
	return ack(t, s, rankAt(time.Second))
}

// rankQueued puts a waiter of class behind the current holder, and reports
// whether queuing it raised the yield ask, which is the observable half of
// higherWaiting at the enqueue call site.
func rankQueued(t *testing.T, s Snapshot, id string, class Class, at time.Time) Snapshot {
	t.Helper()
	next, _ := applyForTest(t, s, request(id, class), at)
	return next
}

func rankQueuedAsked(t *testing.T, s Snapshot, id string, class Class, at time.Time) (Snapshot, bool) {
	t.Helper()
	next, events := applyForTest(t, s, request(id, class), at)
	for _, e := range events {
		if e.Kind == YieldAsked {
			return next, true
		}
	}
	return next, false
}

// TestEachClassHoldsTheBoardForItsOwnCeiling. The ceilings are the durable
// statement of what each kind of work is for: an AI run is an hour, a CI job
// two, a person at a bench eight. They are enforced at the door (enqueue) as
// well as on extension, so an over-long request never enters the queue at all.
func TestEachClassHoldsTheBoardForItsOwnCeiling(t *testing.T) {
	for _, tc := range []struct {
		class   Class
		ceiling time.Duration
	}{
		{ClassAI, time.Hour},
		{ClassCI, 2 * time.Hour},
		{ClassHuman, 8 * time.Hour},
	} {
		if got := classCeiling(tc.class); got != tc.ceiling {
			t.Fatalf("class %d ceiling = %s, want %s", tc.class, got, tc.ceiling)
		}

		at := rankAt(time.Second)
		fits := request("right-at-the-ceiling", tc.class)
		fits.Waiter.Duration = tc.ceiling
		if _, _, err := Apply(boardForTest(t), fits, at); err != nil {
			t.Fatalf("class %d refused a request for exactly its ceiling: %v", tc.class, err)
		}
		over := request("one-nanosecond-over", tc.class)
		over.Waiter.Duration = tc.ceiling + time.Nanosecond
		if _, _, err := Apply(boardForTest(t), over, at); !IsCode(err, InvalidArgument) {
			t.Fatalf("class %d admitted %s: %v", tc.class, over.Waiter.Duration, err)
		}
	}
}

// TestTheCeilingsRiseWithTheClass. The ordering is the thing callers rely on,
// not the three numbers: a class that outranks another may also hold longer.
func TestTheCeilingsRiseWithTheClass(t *testing.T) {
	if !(ClassAI < ClassCI && ClassCI < ClassHuman) {
		t.Fatalf("class ordering changed: AI %d, CI %d, human %d", ClassAI, ClassCI, ClassHuman)
	}
	if !(classCeiling(ClassAI) < classCeiling(ClassCI) && classCeiling(ClassCI) < classCeiling(ClassHuman)) {
		t.Fatalf("ceilings out of order: %s, %s, %s",
			classCeiling(ClassAI), classCeiling(ClassCI), classCeiling(ClassHuman))
	}
}

// TestAnUnknownClassHoldsTheBoardForNoTime is the fail-closed half. The
// default arm is unreachable through every caller today, because validClass
// runs first at all four of them; it is worth holding anyway, because a fourth
// class added without a ceiling entry gets zero, which refuses every duration,
// rather than a permissive fallback nobody notices until a board is held all
// day by work with no stated limit.
func TestAnUnknownClassHoldsTheBoardForNoTime(t *testing.T) {
	for _, class := range []Class{0, ClassHuman + 1, 99, 255} {
		if got := classCeiling(class); got != 0 {
			t.Fatalf("class %d ceiling = %s, want 0", class, got)
		}
		if validClass(class) {
			t.Fatalf("class %d reads as valid", class)
		}
		c := request("unknown-class", class)
		if _, _, err := Apply(boardForTest(t), c, testEpoch); !IsCode(err, InvalidArgument) {
			t.Fatalf("class %d admitted: %v", class, err)
		}
	}
}

// TestNobodyOutranksAFreeBoard. Both predicates read the lease first, so a
// board with waiters and no holder answers false rather than dereferencing a
// nil lease or reporting contention against nobody.
func TestNobodyOutranksAFreeBoard(t *testing.T) {
	free := boardForTest(t)
	free.Queue = []Waiter{{
		ID: "someone", LeaseID: "lease-someone", Holder: "owner-someone",
		Class: ClassHuman, Reason: "test", Duration: time.Hour,
		QueuedAt: testEpoch, Sequence: 1,
	}}
	if higherWaiting(free) {
		t.Fatal("a free board reports being outranked")
	}
	if sameClassHumanWaiting(free) {
		t.Fatal("a free board reports human contention")
	}
}

// TestOnlyAStrictlyHigherClassOutranksTheHolder walks every pair. The
// diagonal is the half that matters: a CI job queued behind a CI job does not
// outrank it, so no yield is asked and the holder runs to its own expiry.
func TestOnlyAStrictlyHigherClassOutranksTheHolder(t *testing.T) {
	classes := []Class{ClassAI, ClassCI, ClassHuman}
	for _, held := range classes {
		for _, waiting := range classes {
			s := rankHeld(t, held)
			s = rankQueued(t, s, "waiter", waiting, rankAt(2*time.Second))
			want := waiting > held
			if got := higherWaiting(s); got != want {
				t.Fatalf("class %d holding, class %d waiting: outranked = %v, want %v", held, waiting, got, want)
			}
		}
	}
}

// TestBeingOutrankedIsNotAboutQueuePosition. The scan is over the whole queue,
// so a single higher waiter behind a crowd of lower ones still outranks.
func TestBeingOutrankedIsNotAboutQueuePosition(t *testing.T) {
	s := rankHeld(t, ClassAI)
	s = rankQueued(t, s, "first-ai", ClassAI, rankAt(2*time.Second))
	s = rankQueued(t, s, "second-ai", ClassAI, rankAt(3*time.Second))
	if higherWaiting(s) {
		t.Fatal("two peers read as outranking the holder")
	}
	s = rankQueued(t, s, "late-human", ClassHuman, rankAt(4*time.Second))
	if !higherWaiting(s) {
		t.Fatalf("a human at the back of the queue does not outrank an AI holder: %#v", s.Queue)
	}
}

// TestOnlyAHumanQueuesAgainstItsOwnClass is the asymmetry, stated once. There
// is no same-class predicate for AI or CI: those wait quietly behind a peer.
// A person behind a person is contention, because the thing being shared is
// somebody's afternoon rather than a scheduler's throughput.
func TestOnlyAHumanQueuesAgainstItsOwnClass(t *testing.T) {
	for _, held := range []Class{ClassAI, ClassCI} {
		s := rankHeld(t, held)
		s = rankQueued(t, s, "peer", held, rankAt(2*time.Second))
		if sameClassHumanWaiting(s) {
			t.Fatalf("class %d peer reads as human contention", held)
		}
	}

	human := rankHeld(t, ClassHuman)
	for _, waiting := range []Class{ClassAI, ClassCI} {
		s := rankQueued(t, human, "machine", waiting, rankAt(2*time.Second))
		if sameClassHumanWaiting(s) {
			t.Fatalf("class %d behind a human reads as human contention", waiting)
		}
		if higherWaiting(s) {
			t.Fatalf("class %d outranks a human holder", waiting)
		}
	}

	s := rankQueued(t, human, "second-person", ClassHuman, rankAt(2*time.Second))
	if !sameClassHumanWaiting(s) {
		t.Fatal("a person behind a person is not contention")
	}
	if higherWaiting(s) {
		t.Fatal("a person outranks a person")
	}
}

// TestAPersonBehindAPersonIsChargedWithoutBeingAskedToYield is what that
// asymmetry actually costs, at the two call sites that read it. Extension
// charges the ten-minute wrap-up budget on EITHER predicate (board.go:704), so
// the second person's wait is paid for; the yield ask is raised on
// higherWaiting ALONE (board.go:576, :540, :411), so the holder is never told
// to stop. Swap either predicate at either site and one of those two halves
// silently changes.
func TestAPersonBehindAPersonIsChargedWithoutBeingAskedToYield(t *testing.T) {
	s := rankHeld(t, ClassHuman)
	s, asked := rankQueuedAsked(t, s, "second-person", ClassHuman, rankAt(2*time.Second))
	if asked {
		t.Fatal("a person behind a person raised a yield ask")
	}
	if s.Phase != Active {
		t.Fatalf("the holder was asked to yield: phase %v, want Active", s.Phase)
	}

	extended, _ := applyForTest(t, s, Extend{
		Actor: "owner-holder", LeaseID: s.Lease.ID, Generation: s.Generation,
		NewExpiry: s.Lease.ExpiresAt.Add(3 * time.Minute), Reason: "finish safely",
	}, rankAt(3*time.Second))
	if extended.Lease.ContendedExtensionUsed != 3*time.Minute {
		t.Fatalf("charge = %s, want 3m0s", extended.Lease.ContendedExtensionUsed)
	}

	// The same extension against an uncontended human board is free, so the
	// charge above is the waiter's doing and not the class's.
	alone := rankHeld(t, ClassHuman)
	free, _ := applyForTest(t, alone, Extend{
		Actor: "owner-holder", LeaseID: alone.Lease.ID, Generation: alone.Generation,
		NewExpiry: alone.Lease.ExpiresAt.Add(3 * time.Minute), Reason: "finish safely",
	}, rankAt(3*time.Second))
	if free.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("uncontended charge = %s, want 0", free.Lease.ContendedExtensionUsed)
	}
}

// TestAPeerCIJobIsNeitherChargedNorAsked is the other side of the same coin,
// and the reading a scheduler depends on: CI queues deeply behind itself all
// day and none of that traffic shortens or charges the job in front.
func TestAPeerCIJobIsNeitherChargedNorAsked(t *testing.T) {
	s := rankHeld(t, ClassCI)
	s, asked := rankQueuedAsked(t, s, "peer-ci", ClassCI, rankAt(2*time.Second))
	if asked {
		t.Fatal("a CI peer raised a yield ask")
	}
	if s.Phase != Active {
		t.Fatalf("a CI peer raised a yield ask: phase %v", s.Phase)
	}
	extended, _ := applyForTest(t, s, Extend{
		Actor: "owner-holder", LeaseID: s.Lease.ID, Generation: s.Generation,
		NewExpiry: s.Lease.ExpiresAt.Add(9 * time.Minute), Reason: "finish safely",
	}, rankAt(3*time.Second))
	if extended.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("a CI peer charged the wrap-up budget: %s", extended.Lease.ContendedExtensionUsed)
	}
}

// TestTheYieldAskFollowsTheHolderRatherThanTheQueue. Cancelling the waiter
// that outranked the holder returns an Active board, and cancelling one of two
// does not, so the ask tracks whether anyone still outranks rather than
// whether the queue ever held someone who did.
func TestTheYieldAskFollowsTheHolderRatherThanTheQueue(t *testing.T) {
	s := rankHeld(t, ClassAI)
	s = rankQueued(t, s, "human-one", ClassHuman, rankAt(2*time.Second))
	s = rankQueued(t, s, "human-two", ClassHuman, rankAt(3*time.Second))
	if s.Phase != YieldRequested {
		t.Fatalf("two humans behind an AI holder did not raise the ask: %v", s.Phase)
	}

	s, _ = applyForTest(t, s, CancelWaiter{Actor: "server", WaiterID: "human-one"}, rankAt(4*time.Second))
	if s.Phase != YieldRequested || !higherWaiting(s) {
		t.Fatalf("the ask dropped while a human was still queued: %v", s.Phase)
	}

	s, _ = applyForTest(t, s, CancelWaiter{Actor: "server", WaiterID: "human-two"}, rankAt(5*time.Second))
	if higherWaiting(s) {
		t.Fatal("nobody is queued and the holder still reads as outranked")
	}
	if s.Phase != Active {
		t.Fatalf("phase %v after the last higher waiter left, want Active", s.Phase)
	}
}
