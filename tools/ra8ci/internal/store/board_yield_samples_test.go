package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The row-shaping half of the recorder, which decides what the estimator later
// reads back. It needs no database, so it is pinned here rather than left to
// an integration run.

func yieldTestCohort() board.YieldCohort {
	return board.YieldCohort{
		BoardID:         "board-1",
		BoardModel:      "ra8p1-ek",
		FixtureRevision: "fixture-c",
		TaskName:        "hil-smoke",
		CatalogDigest:   "2411656a6225954d8f8b6b4a593a79b0b95ddd080c5040515cb0a227f65216e5",
	}
}

func yieldTestBoard(now time.Time) board.Snapshot {
	return board.Snapshot{
		BoardID:        "board-1",
		Phase:          board.Active,
		Generation:     3,
		AgentHighWater: 3,
		Version:        9,
		NextSequence:   2,
		Lease: &board.Lease{
			ID: "lease-held", WaiterID: "waiter-held", Holder: "ci", Class: board.ClassCI,
			Reason: "integration run", Generation: 3, GrantedAt: now.Add(-20 * time.Minute),
			ExpiresAt: now.Add(10 * time.Minute), RequestedDuration: 30 * time.Minute,
			DeadlineVersion: 1,
		},
		Queue: []board.Waiter{{
			ID: "waiter-human", LeaseID: "lease-human", Holder: "brighton", Class: board.ClassHuman,
			Reason: "bench debug", Duration: 30 * time.Minute, QueuedAt: now.Add(-time.Minute), Sequence: 1,
		}},
	}
}

// asked returns the board after a yield request that showed target over the
// standard cohort, which is the state a completing transition is applied to.
func asked(t *testing.T, now time.Time, target time.Duration) board.Snapshot {
	t.Helper()
	after, _, err := board.Apply(yieldTestBoard(now), board.RequestYield{
		Actor: "brighton", WaiterID: "waiter-human", ShownTarget: target, Cohort: yieldTestCohort()}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	return after
}

func TestNoRowForATransitionThatMeasuresNothing(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// A board nobody asked to yield.
	if _, ok, err := yieldSampleRowFor(yieldTestBoard(now), []board.Event{{
		Kind: board.LeaseReleased, LeaseID: "lease-held", At: now, Actor: "ci"}}); ok || err != nil {
		t.Fatalf("ok=%v err=%v, want no row", ok, err)
	}
	// An outstanding yield with no event that ends it either way.
	if _, ok, err := yieldSampleRowFor(asked(t, now, 45*time.Second), []board.Event{{
		Kind: board.DrainStarted, LeaseID: "lease-held", At: now.Add(time.Second), Actor: "ci"}}); ok || err != nil {
		t.Fatalf("ok=%v err=%v, want no row for a drain", ok, err)
	}
}

func TestMeasuredRowCarriesTheRecordedCohortAndPromise(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := asked(t, now, 45*time.Second)
	row, ok, err := yieldSampleRowFor(before, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: "lease-held", At: now.Add(70 * time.Second), Actor: "ci"}})
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v", ok, err)
	}
	if row.Cohort != yieldTestCohort() {
		t.Fatalf("cohort = %+v, want the one recorded on the lease", row.Cohort)
	}
	if row.LeaseID != "lease-held" || row.BoardID != "board-1" || row.WaiterID != "waiter-held" {
		t.Fatalf("identity: lease=%q board=%q waiter=%q", row.LeaseID, row.BoardID, row.WaiterID)
	}
	if row.RequestedAt != now || row.NeutralAt != now.Add(70*time.Second) {
		t.Fatalf("requested=%s neutral=%s", row.RequestedAt, row.NeutralAt)
	}
	if row.ExclusionReason != "" {
		t.Fatalf("a measured row carries an exclusion reason: %q", row.ExclusionReason)
	}
	// The overrun is judged against the number the requester was shown, and
	// the row carries that number so the judgement can be checked later.
	if !row.SafetyOverrun || row.ShownTarget != 45*time.Second {
		t.Fatalf("overrun=%v shown=%s, want an overrun of the promised 45s", row.SafetyOverrun, row.ShownTarget)
	}
}

// A handoff inside the promise is a measurement with no overrun, and the two
// halves of the table's CHECK are exclusive in both directions.
func TestARowIsEitherMeasuredOrCensoredNeverBoth(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := asked(t, now, 45*time.Second)

	row, ok, err := yieldSampleRowFor(before, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: "lease-held", At: now.Add(30 * time.Second), Actor: "ci"}})
	if err != nil || !ok || row.SafetyOverrun || row.ExclusionReason != "" || row.NeutralAt.IsZero() {
		t.Fatalf("inside the promise: row=%+v ok=%v err=%v", row, ok, err)
	}

	for _, tc := range []struct {
		name string
		kind board.EventKind
		want string
	}{
		{"expiry", board.LeaseExpired, board.YieldExcludedExpired},
		{"quarantine", board.BoardQuarantined, board.YieldExcludedQuarantine},
		{"withdrawal", board.YieldCleared, board.YieldExcludedWithdrawn},
	} {
		t.Run(tc.name, func(t *testing.T) {
			row, ok, err := yieldSampleRowFor(before, []board.Event{{
				Kind: tc.kind, LeaseID: "lease-held", At: now.Add(time.Minute), Actor: "server"}})
			if err != nil || !ok {
				t.Fatalf("ok=%v err=%v", ok, err)
			}
			if row.ExclusionReason != tc.want {
				t.Fatalf("reason = %q, want %q", row.ExclusionReason, tc.want)
			}
			if !row.NeutralAt.IsZero() {
				t.Fatalf("a censored row carries a neutral time: %s", row.NeutralAt)
			}
			// An overrun is a claim about a measured latency, so a censored
			// row must never carry one. The table refuses it too.
			if row.SafetyOverrun {
				t.Fatal("a censored row was flagged as a safety overrun")
			}
			// The cohort survives censoring: a request that did not complete
			// is still evidence about that cohort.
			if row.Cohort != yieldTestCohort() {
				t.Fatalf("cohort = %+v", row.Cohort)
			}
		})
	}
}

// A release with no neutral receipt is named apart from any other route into
// recovery: the holder said it was done and could not prove the board was safe.
func TestAReleaseWithNoReceiptIsNamedApart(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	before := asked(t, now, 45*time.Second)
	holder, _, err := yieldSampleRowFor(before, []board.Event{{
		Kind: board.RecoveryNeeded, LeaseID: "lease-held", At: now.Add(time.Minute), Actor: "ci"}})
	if err != nil {
		t.Fatalf("holder recovery: %v", err)
	}
	server, _, err := yieldSampleRowFor(before, []board.Event{{
		Kind: board.RecoveryNeeded, LeaseID: "lease-held", At: now.Add(time.Minute), Actor: "server"}})
	if err != nil {
		t.Fatalf("server recovery: %v", err)
	}
	if holder.ExclusionReason != board.YieldExcludedNoReceipt || server.ExclusionReason != board.YieldExcludedRecovery {
		t.Fatalf("holder=%q server=%q", holder.ExclusionReason, server.ExclusionReason)
	}
}

// A yield the state machine raised itself records no cohort, so there is
// nothing to file the measurement against. That is refused rather than filed
// against a cohort the store would have to invent.
func TestATransitionWithNoRecordedCohortIsRefused(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	raised, _, err := board.Apply(yieldTestBoard(now), board.RequestYield{
		Actor: "brighton", WaiterID: "waiter-human"}, now)
	if err != nil {
		t.Fatalf("request yield: %v", err)
	}
	_, ok, err := yieldSampleRowFor(raised, []board.Event{{
		Kind: board.LeaseReleased, LeaseID: "lease-held", At: now.Add(30 * time.Second), Actor: "ci"}})
	if ok || err == nil {
		t.Fatalf("ok=%v err=%v, want a refusal", ok, err)
	}
	if !board.IsCode(err, board.InvalidArgument) && !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want the missing cohort named", err)
	}
}
