package board

import (
	"testing"
	"time"
)

// Two commands that end a holder's authority, release and the agent-side
// SeedDeadline, each run a guard before they judge anything else. Which guard
// runs first decides what the caller is told went wrong, and a holder that is
// told the wrong thing takes the wrong next step: re-seeding a fence when the
// lease is actually gone, or chasing a token when it simply sent no name.

func endAt(offset time.Duration) time.Time { return testEpoch.Add(offset) }

// endingBoard is an acknowledged, active board held by w-1/l-1 for an hour.
func endingBoard(t *testing.T) Snapshot {
	t.Helper()
	return heldBoard(t, testEpoch, time.Hour)
}

func refusedEnding(t *testing.T, err error, code Code, detail string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected %s: %s, got no error", code, detail)
	}
	if !IsCode(err, code) {
		t.Fatalf("expected code %s, got %v", code, err)
	}
	boardErr, ok := err.(*Error)
	if !ok || boardErr.Detail != detail {
		t.Fatalf("expected detail %q, got %v", detail, err)
	}
}

// --- release ---------------------------------------------------------------

// The identity guard runs ahead of the grant check, so a release that carried
// no holder name is told that rather than that its token went stale. The
// board keeps the lease either way: an unnamed release is not a release.
func TestAReleaseWithNoHolderIdentityIsRefused(t *testing.T) {
	s := endingBoard(t)

	after, events, err := Apply(s, Release{LeaseID: "l-1", Generation: s.Generation, NeutralReceipt: "neutral-ok"}, endAt(time.Minute))
	refusedEnding(t, err, InvalidArgument, "missing holder identity")
	if after.Phase != Active || after.Lease == nil || after.Lease.ID != "l-1" {
		t.Fatalf("unnamed release moved the board: phase %v lease %+v", after.Phase, after.Lease)
	}
	if len(events) != 1 || events[0].Kind != ActionDenied {
		t.Fatalf("expected one denial event, got %+v", events)
	}

	// Nameless AND stale still reads as the missing name.
	_, _, err = Apply(s, Release{LeaseID: "l-gone", Generation: 99, NeutralReceipt: "neutral-ok"}, endAt(time.Minute))
	refusedEnding(t, err, InvalidArgument, "missing holder identity")
}

// A release carries the grant check's own verdict, so a holder presenting a
// superseded token learns the token is stale rather than being told the
// phase is wrong about a board it no longer holds.
func TestAReleaseOfAGrantThatIsNotCurrentIsRefused(t *testing.T) {
	s := endingBoard(t)
	gen := s.Generation

	_, _, err := Apply(s, Release{Actor: "agent-a", LeaseID: "l-other", Generation: gen, NeutralReceipt: "neutral-ok"}, endAt(time.Minute))
	refusedEnding(t, err, StaleGeneration, "lease token is stale")

	_, _, err = Apply(s, Release{Actor: "agent-a", LeaseID: "l-1", Generation: gen + 1, NeutralReceipt: "neutral-ok"}, endAt(time.Minute))
	refusedEnding(t, err, StaleGeneration, "lease token is stale")

	after, _, err := Apply(s, Release{Actor: "agent-a", LeaseID: "l-1", Generation: 0, NeutralReceipt: "neutral-ok"}, endAt(time.Minute))
	refusedEnding(t, err, StaleGeneration, "no matching grant")
	if after.Phase != Active || after.Lease == nil {
		t.Fatalf("refused release moved the board: phase %v lease %+v", after.Phase, after.Lease)
	}
}

// A release that arrives after the deadline is expired by Apply first, and
// the expiry retains the lease, so the holder is told the lease expired
// rather than that no grant exists. The board needs recovery either way: the
// hardware was left in whatever state the deadline interrupted, and a receipt
// that arrives too late cannot vouch for it.
func TestAReleaseAfterTheDeadlineIsRefusedAsExpired(t *testing.T) {
	s := endingBoard(t)
	gen := s.Generation
	rel := Release{Actor: "agent-a", LeaseID: "l-1", Generation: gen, NeutralReceipt: "neutral-ok"}

	after, events, err := Apply(s, rel, s.Lease.ExpiresAt)
	refusedEnding(t, err, Expired, "lease has expired")
	if after.Phase != RecoveryRequired || after.Lease == nil {
		t.Fatalf("expected a retained lease awaiting recovery: phase %v lease %+v", after.Phase, after.Lease)
	}
	if len(events) != 2 || events[0].Kind != LeaseExpired || events[1].Kind != ActionDenied {
		t.Fatalf("expected expiry then denial, got %+v", events)
	}

	intime, _, err := Apply(s, rel, s.Lease.ExpiresAt.Add(-time.Nanosecond))
	if err != nil {
		t.Fatalf("release one nanosecond inside the deadline refused: %v", err)
	}
	if intime.Phase != Ready || intime.Lease != nil {
		t.Fatalf("in-time release did not free the board: phase %v lease %+v", intime.Phase, intime.Lease)
	}
}

// A release with no neutral receipt is accepted as an ending and still sends
// the board to recovery: the holder is done, but nothing vouches for the
// hardware it leaves behind, so the next waiter must not simply be handed it.
func TestAReleaseWithoutANeutralReceiptEndsInRecovery(t *testing.T) {
	s := endingBoard(t)

	after, events, err := Apply(s, Release{Actor: "agent-a", LeaseID: "l-1", Generation: s.Generation}, endAt(time.Minute))
	if err != nil {
		t.Fatalf("receiptless release refused: %v", err)
	}
	if after.Phase != RecoveryRequired || after.Lease == nil {
		t.Fatalf("expected recovery with the lease retained: phase %v lease %+v", after.Phase, after.Lease)
	}
	if len(events) != 1 || events[0].Kind != RecoveryNeeded {
		t.Fatalf("expected one recovery event, got %+v", events)
	}
}

// --- SeedDeadline ----------------------------------------------------------

// An agent seeding a fence under a zero generation or a zero deadline version
// is refused: a fence that cannot say which grant it belongs to would pass
// every later generation check by accident.
func TestSeedingAFenceWithNoGenerationOrVersionIsRefused(t *testing.T) {
	localNow := testEpoch
	expiry := localNow.Add(time.Hour)

	for _, c := range []struct {
		name                string
		generation, version uint64
	}{
		{"zero generation", 0, 1},
		{"zero version", 7, 0},
		{"both zero", 0, 0},
	} {
		fence, err := SeedDeadline(c.generation, c.version, expiry, localNow, time.Second)
		refusedEnding(t, err, InvalidArgument, "zero generation or deadline version")
		if fence != (DeadlineFence{}) {
			t.Fatalf("%s returned a usable fence: %+v", c.name, fence)
		}
	}
}

// The clock is judged before the identifiers, so an agent seeding against an
// expiry that has already passed is told the deadline is gone rather than
// being sent to look at a generation it would have to re-fetch anyway. A
// fence is worth identifying only once there is time left to fence.
func TestAPassedExpiryIsReportedBeforeAMissingGeneration(t *testing.T) {
	localNow := testEpoch

	fence, err := SeedDeadline(0, 0, localNow.Add(-time.Minute), localNow, time.Second)
	refusedEnding(t, err, Expired, "conservative local deadline has passed")
	if fence != (DeadlineFence{}) {
		t.Fatalf("expired seed returned a usable fence: %+v", fence)
	}

	// An unmeasured offset is judged there too, ahead of the identifiers.
	_, err = SeedDeadline(0, 0, localNow.Add(time.Hour), localNow, -time.Second)
	refusedEnding(t, err, InvalidArgument, "invalid or unbounded UTC offset")
}

// A seeded fence is conservative by exactly the offset bound and the safety
// margin, and carries the generation and version it was seeded with, so the
// segment check has something to be stale against.
func TestASeededFenceIsShortenedByTheOffsetAndTheSafetyMargin(t *testing.T) {
	localNow := testEpoch
	expiry := localNow.Add(time.Hour)
	offset := 2 * time.Second

	fence, err := SeedDeadline(7, 3, expiry, localNow, offset)
	if err != nil {
		t.Fatalf("seed refused: %v", err)
	}
	want := expiry.Add(-offset - ClockSafetyMargin)
	if !fence.Until.Equal(want) {
		t.Fatalf("fence until %v, want %v", fence.Until, want)
	}
	if fence.Generation != 7 || fence.Version != 3 {
		t.Fatalf("fence lost its identifiers: %+v", fence)
	}
	if err := fence.CanStartSegment(7, localNow, time.Minute, time.Second); err != nil {
		t.Fatalf("fresh fence refused a one-minute segment: %v", err)
	}
}
