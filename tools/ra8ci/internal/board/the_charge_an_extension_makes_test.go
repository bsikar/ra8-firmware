package board

import (
	"math"
	"testing"
	"time"
)

// An extension is the one command that buys a holder more of the board while
// somebody else is queued for it, so every refusal it can meet is a sentence
// an operator reads at the moment they are being told to stop. These pin all
// of them, and the arithmetic of the contended safe-wrap-up budget: the ten
// minutes a holder may take to finish safely once a waiter outranks them.

func wrapUpAt(d time.Duration) time.Time { return testEpoch.Add(d) }

// wrapUpHeld returns an Active board held by class, with no queue.
func wrapUpHeld(t *testing.T, class Class) Snapshot {
	t.Helper()
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("holder", class), testEpoch)
	return ack(t, s, wrapUpAt(time.Second))
}

// wrapUpContended returns an Active CI board with a human waiter queued, the
// shape that charges the safe-wrap-up budget.
func wrapUpContended(t *testing.T) Snapshot {
	t.Helper()
	s := wrapUpHeld(t, ClassCI)
	s, _ = applyForTest(t, s, request("waiting-human", ClassHuman), wrapUpAt(2*time.Second))
	if !higherWaiting(s) {
		t.Fatalf("fixture does not contend: queue %#v", s.Queue)
	}
	return s
}

func wrapUpExtend(s Snapshot, to time.Time) Extend {
	return Extend{
		Actor: "owner-holder", LeaseID: s.Lease.ID, Generation: s.Generation,
		NewExpiry: to, Reason: "finish safely",
	}
}

func TestAnExtensionWithoutAHolderIdentityIsRefusedAheadOfItsToken(t *testing.T) {
	s := wrapUpHeld(t, ClassCI)
	// Nothing else about the command is right either: no lease, no generation.
	// The identity guard still runs first, so the holder is told what they
	// left out rather than that their grant went stale.
	_, _, err := Apply(s, Extend{NewExpiry: s.Lease.ExpiresAt.Add(time.Minute), Reason: "more"}, wrapUpAt(2*time.Second))
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("nameless extension: %v", err)
	}
}

func TestAnExtensionNamingAnotherLeaseIsStaleRatherThanDenied(t *testing.T) {
	s := wrapUpHeld(t, ClassCI)
	c := wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Minute))
	c.LeaseID = "lease-somebody-else"
	if _, _, err := Apply(s, c, wrapUpAt(2*time.Second)); !IsCode(err, StaleGeneration) {
		t.Fatalf("foreign lease: %v", err)
	}
	c = wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Minute))
	c.Generation = s.Generation + 1
	if _, _, err := Apply(s, c, wrapUpAt(2*time.Second)); !IsCode(err, StaleGeneration) {
		t.Fatalf("later generation: %v", err)
	}
}

func TestAnExtensionNeedsAReasonAndALaterExpiry(t *testing.T) {
	s := wrapUpHeld(t, ClassCI)
	at := wrapUpAt(2 * time.Second)

	noReason := wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Minute))
	noReason.Reason = ""
	if _, _, err := Apply(s, noReason, at); !IsCode(err, InvalidArgument) {
		t.Fatalf("reasonless extension: %v", err)
	}
	if _, _, err := Apply(s, wrapUpExtend(s, time.Time{}), at); !IsCode(err, InvalidArgument) {
		t.Fatalf("zero expiry: %v", err)
	}
	if _, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt), at); !IsCode(err, InvalidArgument) {
		t.Fatalf("same expiry: %v", err)
	}
	if _, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(-time.Minute)), at); !IsCode(err, InvalidArgument) {
		t.Fatalf("earlier expiry: %v", err)
	}
	// One nanosecond later is a later expiry, and the smallest one there is.
	next, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Nanosecond)), at)
	if err != nil {
		t.Fatalf("one nanosecond later refused: %v", err)
	}
	if next.Lease.DeadlineVersion != s.Lease.DeadlineVersion+1 {
		t.Fatalf("deadline version did not move: %d", next.Lease.DeadlineVersion)
	}
}

func TestTheClassCeilingIsMeasuredFromTheGrantAndIsExact(t *testing.T) {
	for _, tc := range []struct {
		class   Class
		ceiling time.Duration
	}{
		{ClassAI, time.Hour},
		{ClassCI, 2 * time.Hour},
		{ClassHuman, 8 * time.Hour},
	} {
		s := wrapUpHeld(t, tc.class)
		at := wrapUpAt(2 * time.Second)
		limit := s.Lease.GrantedAt.Add(tc.ceiling)

		if _, _, err := Apply(s, wrapUpExtend(s, limit.Add(time.Nanosecond)), at); !IsCode(err, Denied) {
			t.Fatalf("class %v: a nanosecond past the ceiling was admitted: %v", tc.class, err)
		}
		// The ceiling itself is reachable, not merely approached.
		next, _, err := Apply(s, wrapUpExtend(s, limit), at)
		if err != nil {
			t.Fatalf("class %v: the ceiling itself was refused: %v", tc.class, err)
		}
		if !next.Lease.ExpiresAt.Equal(limit) {
			t.Fatalf("class %v: expiry %v, want %v", tc.class, next.Lease.ExpiresAt, limit)
		}
		if err := Validate(next); err != nil {
			t.Fatalf("class %v: a lease at its ceiling does not validate: %v", tc.class, err)
		}
	}
}

func TestTheCeilingIsJudgedBeforeTheContendedBudget(t *testing.T) {
	s := wrapUpContended(t)
	// Two minutes of wrap-up is well inside the ten-minute budget, but it
	// lands past the CI ceiling. The holder is told which wall they met, and
	// the budget is not spent on an extension that never happened.
	past := s.Lease.GrantedAt.Add(2*time.Hour + time.Minute)
	next, _, err := Apply(s, wrapUpExtend(s, past), wrapUpAt(3*time.Second))
	if !IsCode(err, Denied) {
		t.Fatalf("past the ceiling under contention: %v", err)
	}
	if next.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("budget charged for a refused extension: %v", next.Lease.ContendedExtensionUsed)
	}
}

func TestAnUncontendedHolderIsChargedNothing(t *testing.T) {
	s := wrapUpHeld(t, ClassCI)
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(30*time.Minute)), wrapUpAt(2*time.Second))
	if s.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("uncontended extension charged %v", s.Lease.ContendedExtensionUsed)
	}
	// And it can go on doing that past ten minutes, because the budget is a
	// limit on making somebody else wait, not a limit on holding a free board.
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(30*time.Minute)), wrapUpAt(3*time.Second))
	if s.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("second uncontended extension charged %v", s.Lease.ContendedExtensionUsed)
	}
}

func TestOnlyAWaiterWhoOutranksTheHolderContends(t *testing.T) {
	// A CI board with an AI waiter: the waiter is below the holder, so the
	// holder is not in anyone's way and nothing is charged.
	s := wrapUpHeld(t, ClassCI)
	s, _ = applyForTest(t, s, request("waiting-ai", ClassAI), wrapUpAt(2*time.Second))
	if higherWaiting(s) || sameClassHumanWaiting(s) {
		t.Fatalf("a lower waiter reads as contention: %#v", s.Queue)
	}
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(20*time.Minute)), wrapUpAt(3*time.Second))
	if s.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("a lower waiter charged the budget: %v", s.Lease.ContendedExtensionUsed)
	}
}

func TestACISiblingDoesNotContendButAHumanSiblingDoes(t *testing.T) {
	// Same class is contention only among humans: sameClassHumanWaiting names
	// one class and not the other, and this is the pair that shows it.
	ci := wrapUpHeld(t, ClassCI)
	ci, _ = applyForTest(t, ci, request("waiting-ci", ClassCI), wrapUpAt(2*time.Second))
	ci, _ = applyForTest(t, ci, wrapUpExtend(ci, ci.Lease.ExpiresAt.Add(20*time.Minute)), wrapUpAt(3*time.Second))
	if ci.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("a CI sibling charged the budget: %v", ci.Lease.ContendedExtensionUsed)
	}

	human := wrapUpHeld(t, ClassHuman)
	human, _ = applyForTest(t, human, request("waiting-human", ClassHuman), wrapUpAt(2*time.Second))
	human, _ = applyForTest(t, human, wrapUpExtend(human, human.Lease.ExpiresAt.Add(4*time.Minute)), wrapUpAt(3*time.Second))
	if human.Lease.ContendedExtensionUsed != 4*time.Minute {
		t.Fatalf("a human sibling charged %v, want 4m", human.Lease.ContendedExtensionUsed)
	}
}

func TestTheContendedBudgetAddsUpAcrossExtensions(t *testing.T) {
	s := wrapUpContended(t)
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(3*time.Minute)), wrapUpAt(3*time.Second))
	if s.Lease.ContendedExtensionUsed != 3*time.Minute {
		t.Fatalf("first charge %v, want 3m", s.Lease.ContendedExtensionUsed)
	}
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(4*time.Minute)), wrapUpAt(4*time.Second))
	if s.Lease.ContendedExtensionUsed != 7*time.Minute {
		t.Fatalf("running charge %v, want 7m", s.Lease.ContendedExtensionUsed)
	}
	// The budget is what is left, so the third extension is measured against
	// the three minutes remaining and not against ten.
	if _, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(3*time.Minute+time.Nanosecond)), wrapUpAt(5*time.Second)); !IsCode(err, Denied) {
		t.Fatalf("spent past the remaining budget: %v", err)
	}
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(3*time.Minute)), wrapUpAt(5*time.Second))
	if s.Lease.ContendedExtensionUsed != 10*time.Minute {
		t.Fatalf("final charge %v, want 10m", s.Lease.ContendedExtensionUsed)
	}
}

func TestTheWholeBudgetIsSpendableInOneExtension(t *testing.T) {
	s := wrapUpContended(t)
	if _, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(10*time.Minute+time.Nanosecond)), wrapUpAt(3*time.Second)); !IsCode(err, Denied) {
		t.Fatalf("a nanosecond past the budget was admitted: %v", err)
	}
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(10*time.Minute)), wrapUpAt(3*time.Second))
	if s.Lease.ContendedExtensionUsed != 10*time.Minute {
		t.Fatalf("charge %v, want 10m", s.Lease.ContendedExtensionUsed)
	}
	// Spent to the last nanosecond means nothing is left, not even that.
	if _, _, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Nanosecond)), wrapUpAt(4*time.Second)); !IsCode(err, Denied) {
		t.Fatalf("a spent budget still bought time: %v", err)
	}
}

func TestAWaiterArrivingLaterStartsChargingTheHolder(t *testing.T) {
	// The charge is decided at the moment of the extension, not at the grant,
	// so a holder who extended freely is charged for what they take after a
	// higher waiter arrives, and is not charged for what they took before.
	s := wrapUpHeld(t, ClassCI)
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(20*time.Minute)), wrapUpAt(2*time.Second))
	if s.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("charged before any waiter: %v", s.Lease.ContendedExtensionUsed)
	}
	s, _ = applyForTest(t, s, request("waiting-human", ClassHuman), wrapUpAt(3*time.Second))
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(6*time.Minute)), wrapUpAt(4*time.Second))
	if s.Lease.ContendedExtensionUsed != 6*time.Minute {
		t.Fatalf("charge after the waiter arrived %v, want 6m", s.Lease.ContendedExtensionUsed)
	}
}

func TestARefusedExtensionSpendsNoneOfTheBudget(t *testing.T) {
	// The reducer charges the budget onto the lease and only afterwards meets
	// the exhausted deadline version, so the charge is on a snapshot that is
	// then refused. Apply rolls the whole command back, which is the only
	// reason a holder is not billed for time they were never given. If that
	// rollback ever goes, this is the test that says so.
	s := wrapUpContended(t)
	s.Lease.DeadlineVersion = math.MaxUint64

	next, events, err := Apply(s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(5*time.Minute)), wrapUpAt(3*time.Second))
	if !IsCode(err, Conflict) {
		t.Fatalf("exhausted deadline version: %v", err)
	}
	if next.Lease.ContendedExtensionUsed != 0 {
		t.Fatalf("refused extension billed %v of the budget", next.Lease.ContendedExtensionUsed)
	}
	if !next.Lease.ExpiresAt.Equal(s.Lease.ExpiresAt) || next.Lease.DeadlineVersion != math.MaxUint64 {
		t.Fatalf("refused extension moved the deadline: %#v", next.Lease)
	}
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("refusal not recorded: %#v", events)
	}
}

func TestEveryPhaseThatAdmitsAnExtensionChargesTheSameWay(t *testing.T) {
	// The three live phases each admit an extension, and which one a holder
	// is in when they ask decides nothing about the charge. Worth stating
	// because the two kinds of contention do not meet the same phases: a
	// higher waiter is auto-asked for on arrival, so an outranked holder is
	// already yielding by the time they extend, while a same-class human
	// waiter raises no ask and leaves the holder Active.
	active := wrapUpHeld(t, ClassHuman)
	active, _ = applyForTest(t, active, request("waiting-human", ClassHuman), wrapUpAt(2*time.Second))
	if active.Phase != Active {
		t.Fatalf("a same-class human waiter moved the phase to %s", active.Phase)
	}
	active, _ = applyForTest(t, active, wrapUpExtend(active, active.Lease.ExpiresAt.Add(time.Minute)), wrapUpAt(3*time.Second))
	if active.Lease.ContendedExtensionUsed != time.Minute {
		t.Fatalf("charge while active %v, want 1m", active.Lease.ContendedExtensionUsed)
	}

	// wrapUpContended is already yielding: enqueue raises the ask as the
	// outranking waiter lands, so no separate RequestYield is needed.
	s := wrapUpContended(t)
	if s.Phase != YieldRequested {
		t.Fatalf("an outranking waiter left the phase at %s", s.Phase)
	}
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(2*time.Minute)), wrapUpAt(4*time.Second))
	if s.Lease.ContendedExtensionUsed != 2*time.Minute {
		t.Fatalf("charge while yielding %v, want 2m", s.Lease.ContendedExtensionUsed)
	}

	s, _ = applyForTest(t, s, BeginDrain{Actor: "owner-holder", LeaseID: s.Lease.ID, Generation: s.Generation}, wrapUpAt(5*time.Second))
	if s.Phase != Draining {
		t.Fatalf("phase after drain: %s", s.Phase)
	}
	// Draining is neutralization, not release: the lease is still the
	// holder's and the same budget is still theirs to spend down.
	s, _ = applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(3*time.Minute)), wrapUpAt(6*time.Second))
	if s.Lease.ContendedExtensionUsed != 5*time.Minute {
		t.Fatalf("charge while draining %v, want 5m", s.Lease.ContendedExtensionUsed)
	}
}

func TestAnAcceptedExtensionFilesOneEventAndMovesOneVersion(t *testing.T) {
	s := wrapUpContended(t)
	before := s.Lease.DeadlineVersion
	next, events := applyForTest(t, s, wrapUpExtend(s, s.Lease.ExpiresAt.Add(time.Minute)), wrapUpAt(3*time.Second))
	if len(events) != 1 || events[0].Kind != LeaseExtended {
		t.Fatalf("extension events: %#v", events)
	}
	if events[0].Reason != "finish safely" {
		t.Fatalf("the holder's reason did not survive: %q", events[0].Reason)
	}
	if next.Lease.DeadlineVersion != before+1 {
		t.Fatalf("deadline version %d, want %d", next.Lease.DeadlineVersion, before+1)
	}
	if next.Version != s.Version+1 {
		t.Fatalf("snapshot version %d, want %d", next.Version, s.Version+1)
	}
}
