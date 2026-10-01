package board

import (
	"strings"
	"testing"
	"time"
)

// The deadline fence is the agent's own authority to touch a board, held in
// local monotonic time so a server it cannot reach, and a wall clock it cannot
// trust, can neither extend it nor cut it short. Every segment the agent runs
// passes through DeadlineFence.CanStartSegment twice: once before the work is
// requested (agent.go:168) and once more with the time already spent deducted
// before the operation is actually spawned (agent.go:264). It is the last
// check between a decision and a probe moving on a bench nobody is watching.
//
// TestDeadlineFenceFailClosedAndMonotonic pins the seeding arithmetic and two
// of the four answers this method gives: the segment that fits, and the one
// that is longer than the remaining budget. The other two are the ones that
// decide what happens when the fence is absent or already spent, and those are
// the readings an agent reaches in exactly the situations it must not act in.
// This file holds all four, plus the boundary each of them turns on.

func fenceMoment() time.Time {
	return time.Date(2026, 3, 9, 14, 0, 0, 0, time.UTC)
}

// segmentFence is a fence with a known amount of local authority left, built
// through SeedDeadline rather than by hand so the budget is the one the agent
// would actually be carrying.
func segmentFence(t *testing.T, generation uint64, budget time.Duration) (DeadlineFence, time.Time) {
	t.Helper()
	now := fenceMoment()
	fence, err := SeedDeadline(generation, 1, now.Add(budget+MaxClockOffset+ClockSafetyMargin), now, MaxClockOffset)
	if err != nil {
		t.Fatalf("seed fence: %v", err)
	}
	if got := fence.Until.Sub(now); got != budget {
		t.Fatalf("fence budget = %s, want %s", got, budget)
	}
	return fence, now
}

// A segment whose bound and recovery margin together fit inside the remaining
// authority runs. This is the reading every other case is measured against.
func TestASegmentInsideTheRemainingAuthorityRuns(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	if err := fence.CanStartSegment(7, now, 30*time.Second, 10*time.Second); err != nil {
		t.Fatalf("segment with 20s of slack denied: %v", err)
	}
}

// The boundary the Deadline refusal turns on. The check is
// bound > Until.Sub(localNow) - recoveryMargin, so a segment that consumes the
// budget down to exactly its recovery margin is admitted and one nanosecond
// more is not. Which side the equal case falls on is the difference between a
// flash that is allowed to start with its whole recovery window intact and one
// refused for using all of the time it was given.
func TestASegmentEndingExactlyOnItsRecoveryMarginRuns(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	margin := 10 * time.Second
	exact := time.Minute - margin
	if err := fence.CanStartSegment(7, now, exact, margin); err != nil {
		t.Fatalf("segment ending exactly on its margin denied: %v", err)
	}
	if err := fence.CanStartSegment(7, now, exact+time.Nanosecond, margin); !IsCode(err, Deadline) {
		t.Fatalf("segment one nanosecond past its margin accepted: %v", err)
	}
}

// A segment that fits the remaining time on its own but not once the recovery
// margin is reserved is refused, and refused as Deadline rather than Expired:
// the authority has not run out, there is just not enough of it left to both
// do the work and put the board back in a known-safe state afterwards.
func TestASegmentThatCrowdsOutItsRecoveryMarginIsRefused(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	err := fence.CanStartSegment(7, now, 55*time.Second, 10*time.Second)
	if !IsCode(err, Deadline) {
		t.Fatalf("segment crowding out its recovery margin: %v, want Deadline", err)
	}
	if IsCode(err, Expired) {
		t.Fatalf("a live fence reported as expired: %v", err)
	}
}

// A margin of zero is a legitimate ask (agent.go:186 uses one for the liveness
// probe), so the whole remaining budget is available to the segment.
func TestAZeroRecoveryMarginLeavesTheWholeBudget(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	if err := fence.CanStartSegment(7, now, time.Minute, 0); err != nil {
		t.Fatalf("segment filling the budget with no margin denied: %v", err)
	}
	if err := fence.CanStartSegment(7, now, time.Minute+time.Nanosecond, 0); !IsCode(err, Deadline) {
		t.Fatalf("segment past the budget with no margin accepted: %v", err)
	}
}

// Local time at or after the deadline is Expired, and the boundary is exact:
// the instant the deadline names is already past it. A fence is authority to
// act *before* Until, never at it.
func TestAFenceIsSpentAtTheInstantItNames(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	if err := fence.CanStartSegment(7, fence.Until.Add(-time.Nanosecond), time.Nanosecond, 0); err != nil {
		t.Fatalf("segment one nanosecond before the deadline denied: %v", err)
	}
	if err := fence.CanStartSegment(7, fence.Until, time.Nanosecond, 0); !IsCode(err, Expired) {
		t.Fatalf("segment at the deadline accepted: %v", err)
	}
	if err := fence.CanStartSegment(7, fence.Until.Add(time.Hour), time.Nanosecond, 0); !IsCode(err, Expired) {
		t.Fatalf("segment long past the deadline accepted: %v", err)
	}
	_ = now
}

// The zero-value fence is what an agent holds before it has seeded one, and
// what agent.go replaces on every refresh. It authorizes nothing, and it is
// refused as StaleGeneration rather than Expired so the caller is told the
// fence is absent rather than that its lease ran out.
func TestTheZeroFenceAuthorizesNothing(t *testing.T) {
	var absent DeadlineFence
	err := absent.CanStartSegment(7, fenceMoment(), time.Second, 0)
	if !IsCode(err, StaleGeneration) {
		t.Fatalf("zero fence: %v, want StaleGeneration", err)
	}
}

// A fence carrying a generation other than the one the token names is stale.
// The generation moves when the board is recovered or re-fenced, so a fence
// from before that is authority over a board that has since been handed to
// somebody else, in either direction.
func TestAFenceFromAnotherGenerationIsStale(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	for _, generation := range []uint64{6, 8, 99} {
		if err := fence.CanStartSegment(generation, now, time.Second, 0); !IsCode(err, StaleGeneration) {
			t.Fatalf("fence for generation 7 accepted generation %d: %v", generation, err)
		}
	}
}

// A fence with time left on it but no generation at all cannot be argued into
// authorizing a segment, including by a caller naming generation zero, which
// is caught by the argument guard first.
func TestAFenceWithNoGenerationIsRefusedEitherWay(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	if err := fence.CanStartSegment(0, now, time.Second, 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("generation zero: %v, want InvalidArgument", err)
	}
	unfenced := DeadlineFence{Version: 1, Until: fence.Until}
	if err := unfenced.CanStartSegment(7, now, time.Second, 0); !IsCode(err, StaleGeneration) {
		t.Fatalf("fence with no generation: %v, want StaleGeneration", err)
	}
}

// The argument guard. Each of these is a caller mistake rather than a verdict
// about the board, so each is InvalidArgument and none of them is allowed to
// read as a live or a spent lease.
func TestTheArgumentGuardRefusesBeforeItJudgesTheFence(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	cases := []struct {
		name       string
		generation uint64
		localNow   time.Time
		bound      time.Duration
		margin     time.Duration
	}{
		{"zero generation", 0, now, time.Second, 0},
		{"zero local time", 7, time.Time{}, time.Second, 0},
		{"zero bound", 7, now, 0, 0},
		{"negative bound", 7, now, -time.Second, 0},
		{"negative margin", 7, now, time.Second, -time.Second},
	}
	for _, c := range cases {
		err := fence.CanStartSegment(c.generation, c.localNow, c.bound, c.margin)
		if !IsCode(err, InvalidArgument) {
			t.Fatalf("%s: %v, want InvalidArgument", c.name, err)
		}
	}
}

// The guard runs first on purpose. A bad argument against a fence that is also
// stale and also spent still reads as the caller's mistake, so a segment is
// never refused for the wrong reason and an operator is never sent to recover
// a board over a call that was malformed.
func TestABadArgumentIsNotReportedAsAStaleOrSpentFence(t *testing.T) {
	var absent DeadlineFence
	err := absent.CanStartSegment(0, time.Time{}, 0, -time.Second)
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("malformed call against an absent fence: %v, want InvalidArgument", err)
	}
	spent, now := segmentFence(t, 7, time.Minute)
	if err := spent.CanStartSegment(9, now.Add(time.Hour), 0, 0); !IsCode(err, InvalidArgument) {
		t.Fatalf("malformed call against a spent, mismatched fence: %v, want InvalidArgument", err)
	}
}

// The order of the two verdicts that are both about the fence. A fence from
// another generation that has also run out reads as stale, because the
// generation says the fence is not this lease's at all and its clock is
// therefore not evidence about this lease's remaining time.
func TestAStaleFenceIsNotReportedAsAnExpiredOne(t *testing.T) {
	fence, _ := segmentFence(t, 7, time.Minute)
	err := fence.CanStartSegment(8, fence.Until.Add(time.Hour), time.Second, 0)
	if !IsCode(err, StaleGeneration) {
		t.Fatalf("stale and spent fence: %v, want StaleGeneration", err)
	}
}

// No refusal from this method may read as an expiry unless it is one. The
// agent surfaces these details to an operator deciding whether a board needs
// recovering, and "the deadline passed" is the reading that sends somebody to
// a bench.
func TestOnlyTheExpiredRefusalTalksAboutAPassedDeadline(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	notExpired := []struct {
		name string
		err  error
	}{
		{"stale generation", fence.CanStartSegment(8, now, time.Second, 0)},
		{"absent fence", DeadlineFence{}.CanStartSegment(7, now, time.Second, 0)},
		{"bad argument", fence.CanStartSegment(7, now, 0, 0)},
		{"insufficient time", fence.CanStartSegment(7, now, 55*time.Second, 10*time.Second)},
	}
	for _, c := range notExpired {
		if c.err == nil {
			t.Fatalf("%s: admitted", c.name)
		}
		if strings.Contains(c.err.Error(), "deadline has passed") {
			t.Fatalf("%s reads as an expiry: %v", c.name, c.err)
		}
	}
	expired := fence.CanStartSegment(7, fence.Until, time.Second, 0)
	if !strings.Contains(expired.Error(), "deadline has passed") {
		t.Fatalf("the expiry refusal does not say so: %v", expired)
	}
}

// The fence is read-only. Nothing about asking it a question, including the
// questions it refuses, may move the authority it carries.
func TestAskingTheFenceDoesNotMoveIt(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	before := fence
	_ = fence.CanStartSegment(7, now, 30*time.Second, 10*time.Second)
	_ = fence.CanStartSegment(8, now, 30*time.Second, 10*time.Second)
	_ = fence.CanStartSegment(7, fence.Until, time.Second, 0)
	_ = fence.CanStartSegment(0, time.Time{}, 0, -time.Second)
	if fence != before {
		t.Fatalf("fence moved under questioning: %#v, was %#v", fence, before)
	}
}

// The two checks the agent runs are the same method with the time already
// spent deducted (agent.go:264 passes bound minus elapsed). Time passing on
// its own can never flip the verdict, because the remaining budget and the
// remaining bound shrink at exactly the same rate, so the slack between them
// is constant. That is worth pinning rather than assuming: it says the second
// check is not a re-run of the first against a later clock, and the only
// thing it can catch is authority that moved underneath the segment.
func TestElapsedTimeAloneNeverFlipsTheSecondCheck(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	bound := 40 * time.Second
	margin := 10 * time.Second
	if err := fence.CanStartSegment(7, now, bound, margin); err != nil {
		t.Fatalf("first check denied: %v", err)
	}
	for _, spent := range []time.Duration{time.Second, 15 * time.Second, 35 * time.Second, bound - time.Nanosecond} {
		if err := fence.CanStartSegment(7, now.Add(spent), bound-spent, margin); err != nil {
			t.Fatalf("second check denied after %s spent: %v", spent, err)
		}
	}
}

// What the second check does catch: a fence that shortened between the two
// calls. RefreshDeadline runs on every status read and can only shorten a
// heartbeat deadline, so a server that re-measured its offset or moved the
// expiry in can leave an agent holding less authority than the segment was
// authorized against. The second call is the only place that is noticed
// before the probe moves.
func TestTheSecondCheckCatchesAFenceThatShortenedUnderneath(t *testing.T) {
	fence, now := segmentFence(t, 7, time.Minute)
	bound := 40 * time.Second
	margin := 10 * time.Second
	if err := fence.CanStartSegment(7, now, bound, margin); err != nil {
		t.Fatalf("first check denied: %v", err)
	}
	spent := 10 * time.Second
	shortened, err := RefreshDeadline(fence, 7, 1, now.Add(spent+20*time.Second+MaxClockOffset+ClockSafetyMargin), now.Add(spent), MaxClockOffset, false)
	if err != nil {
		t.Fatalf("refresh: %v", err)
	}
	if !shortened.Until.Before(fence.Until) {
		t.Fatalf("refresh did not shorten the fence: %s, was %s", shortened.Until, fence.Until)
	}
	if err := shortened.CanStartSegment(7, now.Add(spent), bound-spent, margin); !IsCode(err, Deadline) {
		t.Fatalf("shortened fence admitted the rest of the segment: %v", err)
	}
}

// A heartbeat that arrives with more room than the fence holds cannot lengthen
// it, so it cannot talk the second check into admitting a segment the fence
// had already run out of room for. RefreshDeadline enforces that without an
// authorized extension; this holds the consequence at the segment gate.
func TestAHeartbeatCannotBuyBackRoomForASegment(t *testing.T) {
	fence, now := segmentFence(t, 7, 20*time.Second)
	if err := fence.CanStartSegment(7, now, 30*time.Second, 0); !IsCode(err, Deadline) {
		t.Fatalf("oversized segment admitted: %v", err)
	}
	refreshed, err := RefreshDeadline(fence, 7, 1, now.Add(time.Hour), now, MaxClockOffset, false)
	if err != nil {
		t.Fatalf("refresh: %v", err)
	}
	if refreshed.Until != fence.Until {
		t.Fatalf("heartbeat moved the deadline to %s, was %s", refreshed.Until, fence.Until)
	}
	if err := refreshed.CanStartSegment(7, now, 30*time.Second, 0); !IsCode(err, Deadline) {
		t.Fatalf("heartbeat bought room for an oversized segment: %v", err)
	}
}
