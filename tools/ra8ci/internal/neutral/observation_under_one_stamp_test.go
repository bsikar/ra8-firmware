// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"testing"
	"time"
)

// readWindow is one bracketed physical read: the clock before the first
// reading and the clock after the last.
func readWindow(span time.Duration) (time.Time, time.Time) {
	startedAt := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	return startedAt, startedAt.Add(span)
}

// steppingClock hands out the given stamps in order and repeats the last one,
// so a fixture can make a read appear to take as long as it likes.
func steppingClock(stamps ...time.Time) func() time.Time {
	var calls int
	return func() time.Time {
		at := stamps[len(stamps)-1]
		if calls < len(stamps) {
			at = stamps[calls]
		}
		calls++
		return at
	}
}

func TestObservationStampIsTheStartOfItsWindow(t *testing.T) {
	startedAt, finishedAt := readWindow(2 * time.Second)
	stamp, err := observationUnderOneStamp(startedAt, finishedAt)
	if err != nil {
		t.Fatalf("bracketed read refused: %v", err)
	}
	if !stamp.Equal(startedAt) {
		t.Fatalf("stamp %s is not the start of the window %s", stamp, startedAt)
	}
}

func TestObservationStampAcceptsAnInstantRead(t *testing.T) {
	startedAt, _ := readWindow(0)
	stamp, err := observationUnderOneStamp(startedAt, startedAt)
	if err != nil || !stamp.Equal(startedAt) {
		t.Fatalf("instant read refused: stamp=%s err=%v", stamp, err)
	}
}

func TestObservationStampAcceptsTheWholeBudget(t *testing.T) {
	startedAt, finishedAt := readWindow(maxObservationAge)
	if _, err := observationUnderOneStamp(startedAt, finishedAt); err != nil {
		t.Fatalf("read of exactly the budget refused: %v", err)
	}
}

func TestObservationStampRefusesOneNanosecondPastTheBudget(t *testing.T) {
	startedAt, finishedAt := readWindow(maxObservationAge + time.Nanosecond)
	if _, err := observationUnderOneStamp(startedAt, finishedAt); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("read past the budget accepted: %v", err)
	}
}

func TestObservationStampRefusesLongReads(t *testing.T) {
	for _, span := range []time.Duration{6 * time.Second, 30 * time.Second, 90 * time.Second, time.Hour} {
		startedAt, finishedAt := readWindow(span)
		stamp, err := observationUnderOneStamp(startedAt, finishedAt)
		if !errors.Is(err, ErrObservationAbsent) || !stamp.IsZero() {
			t.Fatalf("read spanning %s accepted: stamp=%s err=%v", span, stamp, err)
		}
	}
}

func TestObservationStampRefusesAnInvertedPair(t *testing.T) {
	startedAt, _ := readWindow(0)
	if _, err := observationUnderOneStamp(startedAt, startedAt.Add(-time.Nanosecond)); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("read finishing before it began accepted: %v", err)
	}
}

func TestObservationStampRefusesAMissingStamp(t *testing.T) {
	startedAt, finishedAt := readWindow(time.Second)
	cases := map[string][2]time.Time{
		"neither":  {time.Time{}, time.Time{}},
		"no_start": {time.Time{}, finishedAt},
		"no_end":   {startedAt, time.Time{}},
	}
	for name, pair := range cases {
		if _, err := observationUnderOneStamp(pair[0], pair[1]); !errors.Is(err, ErrObservationAbsent) {
			t.Fatalf("%s: unbracketed read accepted: %v", name, err)
		}
	}
}

func TestObservationStampComesBackInUTC(t *testing.T) {
	zone := time.FixedZone("UTC-6", -6*60*60)
	startedAt := time.Date(2026, 9, 23, 6, 0, 0, 0, zone)
	stamp, err := observationUnderOneStamp(startedAt, startedAt.Add(time.Second))
	if err != nil {
		t.Fatalf("bracketed read refused: %v", err)
	}
	if stamp.Location() != time.UTC || !stamp.Equal(startedAt) {
		t.Fatalf("stamp %s in %s is not the same instant in UTC", stamp, stamp.Location())
	}
}

// The stamp this rule returns has to survive the rules it feeds: a read that
// fills the whole budget still leaves a signature at the end of it inside
// checkObservationIsFresh only because the span is bounded.
func TestObservationStampFeedsTheFreshnessRule(t *testing.T) {
	startedAt, finishedAt := readWindow(maxObservationAge)
	stamp, err := observationUnderOneStamp(startedAt, finishedAt)
	if err != nil {
		t.Fatalf("read of exactly the budget refused: %v", err)
	}
	if !checkObservationIsFresh(stamp, finishedAt) {
		t.Fatal("a stamp this rule allows is already too stale to sign")
	}
	if checkObservationIsFresh(stamp, finishedAt.Add(time.Nanosecond)) {
		t.Fatal("freshness rule accepted a signature past the budget")
	}
}

func TestLinuxObserverStampsTheStartOfItsRead(t *testing.T) {
	observer, gate, now, challenge := observerFixture(t)
	observer.now = steppingClock(now, now.Add(2*time.Second))
	observation, err := observer.ObserveNeutral(context.Background(), challenge)
	if err != nil {
		t.Fatalf("neutral observation rejected: %v", err)
	}
	if !observation.ObservedAt.Equal(now) {
		t.Fatalf("observation stamped %s, not the start of its read %s", observation.ObservedAt, now)
	}
	if gate.locked {
		t.Fatal("gate remained locked after a stamped observation")
	}
}

func TestLinuxObserverRefusesAReadLongerThanTheBudget(t *testing.T) {
	observer, gate, now, challenge := observerFixture(t)
	observer.now = steppingClock(now, now.Add(maxObservationAge+time.Millisecond))
	observation, err := observer.ObserveNeutral(context.Background(), challenge)
	if !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("read longer than the budget accepted: %v", err)
	}
	if observation.Neutral || !observation.ObservedAt.IsZero() || len(observation.Evidence) != 0 {
		t.Fatalf("refused read still returned an observation: %+v", observation)
	}
	if gate.locked {
		t.Fatal("gate remained locked after a refused read")
	}
}

// A ninety-second read is the shape the old end-of-read stamp hid: every
// reading in it is long stale, and the receipt would have looked fresh.
func TestLinuxObserverRefusesTheSlowProcfsSweep(t *testing.T) {
	observer, gate, now, challenge := observerFixture(t)
	observer.now = steppingClock(now, now.Add(90*time.Second))
	if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrObservationAbsent) {
		t.Fatalf("ninety-second read accepted: %v", err)
	}
	if gate.locked {
		t.Fatal("gate remained locked after a refused read")
	}
}
