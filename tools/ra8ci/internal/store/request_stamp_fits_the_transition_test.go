// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// The request stamp is the one stamp that survives every exclusion, so these
// run mostly through the recorder's own shaping half, where a censored row is
// built. yieldTestBoard, yieldTestCohort and asked come from
// board_yield_samples_test.go.

// ending is the transition that ends a handoff for the held lease at at.
func ending(kind board.EventKind, at time.Time, actor string) []board.Event {
	return []board.Event{{Kind: kind, LeaseID: "lease-held", At: at, Actor: actor}}
}

func TestRequestStampAcceptsAnOrdinaryCensoredHandoff(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	row, ok, err := yieldSampleRowFor(asked(t, now, 45*time.Second),
		ending(board.LeaseExpired, now.Add(2*time.Minute), "server"))
	if err != nil || !ok {
		t.Fatalf("ok=%v err=%v, want the censored row", ok, err)
	}
	if row.ExclusionReason == "" {
		t.Fatalf("row is measured, want censored")
	}
	if row.RequestedAt != now {
		t.Fatalf("requested = %s, want %s", row.RequestedAt, now)
	}
}

func TestRequestStampRefusesACensoredRowRequestedAfterItEnded(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// The host that recorded the request is an hour ahead of the host
	// committing the expiry. Nothing in board looks at this: a sample with an
	// exclusion reason leaves validateYieldSample on its first line.
	_, ok, err := yieldSampleRowFor(asked(t, now, 45*time.Second),
		ending(board.LeaseExpired, now.Add(-time.Hour), "server"))
	if ok || !errors.Is(err, ErrConflict) {
		t.Fatalf("ok=%v err=%v, want a conflict", ok, err)
	}
}

func TestRequestStampToleranceIsThePlanesClockOffset(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// Exactly the tolerated offset: the request stands.
	atTheEdge := now.Add(-board.MaxClockOffset)
	if _, ok, err := yieldSampleRowFor(asked(t, now, 0),
		ending(board.YieldCleared, atTheEdge, "brighton")); !ok || err != nil {
		t.Fatalf("ok=%v err=%v, want the row at the tolerance edge", ok, err)
	}
	// One nanosecond past it: refused.
	if _, ok, err := yieldSampleRowFor(asked(t, now, 0),
		ending(board.YieldCleared, atTheEdge.Add(-time.Nanosecond), "brighton")); ok || !errors.Is(err, ErrConflict) {
		t.Fatalf("ok=%v err=%v, want a conflict one nanosecond past the tolerance", ok, err)
	}
}

func TestRequestStampRefusesEveryCensoringRoute(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	ahead := now.Add(-time.Hour)
	for _, route := range []struct {
		name  string
		kind  board.EventKind
		actor string
	}{
		{"expired", board.LeaseExpired, "server"},
		{"recovery", board.RecoveryNeeded, "server"},
		{"no receipt", board.RecoveryNeeded, "ci"},
		{"quarantined", board.BoardQuarantined, "server"},
		{"withdrawn", board.YieldCleared, "brighton"},
	} {
		t.Run(route.name, func(t *testing.T) {
			if _, ok, err := yieldSampleRowFor(asked(t, now, 0),
				ending(route.kind, ahead, route.actor)); ok || !errors.Is(err, ErrConflict) {
				t.Fatalf("ok=%v err=%v, want a conflict", ok, err)
			}
		})
	}
}

func TestMeasuredRowIsAlreadyCoveredByTheNeutralStamp(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// A release stamped before the request: board refuses it on the ordering
	// rule, before this one is reached. Pinned so the redundancy stays
	// deliberate rather than accidental.
	_, ok, err := yieldSampleRowFor(asked(t, now, 45*time.Second),
		ending(board.LeaseReleased, now.Add(-time.Hour), "ci"))
	if ok || err == nil {
		t.Fatalf("ok=%v err=%v, want a refusal", ok, err)
	}
	// And an ordinary release still shapes its row.
	if _, ok, err := yieldSampleRowFor(asked(t, now, 45*time.Second),
		ending(board.LeaseReleased, now.Add(70*time.Second), "ci")); !ok || err != nil {
		t.Fatalf("ok=%v err=%v, want the measured row", ok, err)
	}
}

func TestRequestStampIgnoresAnotherLeasesTerminalEvent(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// A transition that ends this handoff and grants the next lease: only the
	// event naming this lease may be judged against.
	events := []board.Event{
		{Kind: board.LeaseExpired, LeaseID: "lease-other", At: now.Add(-time.Hour), Actor: "server"},
		{Kind: board.LeaseExpired, LeaseID: "lease-held", At: now.Add(time.Minute), Actor: "server"},
	}
	if _, ok, err := yieldSampleRowFor(asked(t, now, 0), events); !ok || err != nil {
		t.Fatalf("ok=%v err=%v, want the row judged against this lease's own end", ok, err)
	}
}

func TestRequestStampTakesTheFirstEndNotTheLast(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// The same lease reaching neutral and then being quarantined: the first
	// event ends the handoff, so a later stamp cannot rescue a request the
	// first one cannot account for.
	events := []board.Event{
		{Kind: board.YieldCleared, LeaseID: "lease-held", At: now.Add(-time.Hour), Actor: "brighton"},
		{Kind: board.BoardQuarantined, LeaseID: "lease-held", At: now.Add(time.Hour), Actor: "server"},
	}
	if _, ok, err := yieldSampleRowFor(asked(t, now, 0), events); ok || !errors.Is(err, ErrConflict) {
		t.Fatalf("ok=%v err=%v, want the first end to decide", ok, err)
	}
}

func TestRequestStampRefusesAnUnstampedEnd(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	if err := yieldRequestStampFitsTheTransition(
		yieldSampleRow{LeaseID: "lease-held", RequestedAt: now},
		ending(board.LeaseExpired, time.Time{}, "server")); !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want a conflict for an unstamped end", err)
	}
}

func TestRequestStampNeedsATransitionToJudgeAgainst(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// Reachable only by calling the rule directly: the recorder files nothing
	// without a terminal event. It must not read the zero time as an end.
	if err := yieldRequestStampFitsTheTransition(
		yieldSampleRow{LeaseID: "lease-held", RequestedAt: now},
		[]board.Event{{Kind: board.DrainStarted, LeaseID: "lease-held", At: now, Actor: "ci"}}); !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want a conflict with no end in the transition", err)
	}
}

func TestTheWriteRefusesWhatTheReadWouldRefuseForever(t *testing.T) {
	now := time.Date(2026, 9, 24, 18, 0, 0, 0, time.UTC)
	// The row this rule exists to keep out of the table, shown through the
	// read that would refuse it: requested_at is the read's ORDER BY key and a
	// future stamp never falls past the cutoff, so it holds the head of every
	// page of its cohort.
	ahead := yieldSampleRow{
		LeaseID:         "lease-held",
		BoardID:         "board-1",
		Cohort:          yieldTestCohort(),
		RequestedAt:     now.Add(24 * time.Hour),
		ExclusionReason: board.YieldExcludedExpired,
	}
	if err := yieldSampleFitsTheClock(ahead, now); err == nil {
		t.Fatalf("the read accepts a row stamped a day ahead; this rule's premise is gone")
	}
	if !ahead.RequestedAt.After(yieldHistoryCutoff(now)) {
		t.Fatalf("the read's cutoff drops a future row after all; this rule's premise is gone")
	}
	if err := yieldRequestStampFitsTheTransition(ahead,
		ending(board.LeaseExpired, now, "server")); !errors.Is(err, ErrConflict) {
		t.Fatalf("err = %v, want the write to refuse it", err)
	}
}
