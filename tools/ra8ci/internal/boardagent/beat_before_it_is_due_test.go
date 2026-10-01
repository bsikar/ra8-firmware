// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
)

func dueAt(base time.Time, after time.Duration) boardclient.HolderLiveness {
	return boardclient.HolderLiveness{Held: true, NextBeatBy: base.Add(after)}
}

func TestNoDueStampLeavesTheCadenceAlone(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	if got := nextBeatWait(2*time.Second, time.Time{}, now); got != 2*time.Second {
		t.Fatalf("wait without a due stamp = %v, want the cadence 2s", got)
	}
}

func TestDueStampBeyondTheCadenceLeavesItAlone(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	if got := nextBeatWait(2*time.Second, now.Add(time.Minute), now); got != 2*time.Second {
		t.Fatalf("wait with a distant due stamp = %v, want the cadence 2s", got)
	}
}

func TestDueStampExactlyAtTheCadenceLeavesItAlone(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	if got := nextBeatWait(2*time.Second, now.Add(2*time.Second), now); got != 2*time.Second {
		t.Fatalf("wait with the due stamp at the cadence = %v, want 2s", got)
	}
}

func TestDueStampInsideTheCadenceShortensTheWait(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	got := nextBeatWait(10*time.Second, now.Add(4*time.Second), now)
	if got != 4*time.Second {
		t.Fatalf("wait with the due stamp inside the cadence = %v, want 4s", got)
	}
}

func TestDueStampAlreadyPassedStillHoldsTheFloor(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	got := nextBeatWait(time.Minute, now.Add(-time.Hour), now)
	if got != minHeartbeatInterval {
		t.Fatalf("wait with a passed due stamp = %v, want the floor %v", got, minHeartbeatInterval)
	}
}

func TestDueStampInsideTheFloorHoldsTheFloor(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	got := nextBeatWait(time.Minute, now.Add(minHeartbeatInterval-time.Millisecond), now)
	if got != minHeartbeatInterval {
		t.Fatalf("wait with the due stamp inside the floor = %v, want the floor %v", got, minHeartbeatInterval)
	}
}

func TestDueStampAtTheFloorIsKept(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	got := nextBeatWait(time.Minute, now.Add(minHeartbeatInterval), now)
	if got != minHeartbeatInterval {
		t.Fatalf("wait with the due stamp at the floor = %v, want %v", got, minHeartbeatInterval)
	}
}

func TestWaitNeverExceedsTheCadenceTheServerNamed(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	for _, remaining := range []time.Duration{-time.Hour, 0, time.Millisecond, time.Second, 5 * time.Second, time.Hour} {
		if got := nextBeatWait(2*time.Second, now.Add(remaining), now); got > 2*time.Second {
			t.Fatalf("remaining %v produced wait %v, above the cadence 2s", remaining, got)
		}
	}
}

func TestWaitNeverDropsBelowTheFloor(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	for _, remaining := range []time.Duration{-time.Hour, 0, time.Nanosecond, time.Millisecond, time.Second} {
		if got := nextBeatWait(time.Minute, now.Add(remaining), now); got < minHeartbeatInterval {
			t.Fatalf("remaining %v produced wait %v, below the floor %v", remaining, got, minHeartbeatInterval)
		}
	}
}

func TestAnsweredBeatReplacesTheDueStamp(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	previous := now.Add(time.Second)
	got := dueStamp(previous, dueAt(now, 30*time.Second), true)
	if !got.Equal(now.Add(30 * time.Second)) {
		t.Fatalf("answered beat kept %v, want the newly reported stamp", got)
	}
}

func TestAnsweredBeatNamingNoStampClearsTheOldOne(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	got := dueStamp(now.Add(time.Second), boardclient.HolderLiveness{Held: true}, true)
	if !got.IsZero() {
		t.Fatalf("answered beat naming no stamp left %v, want none", got)
	}
}

func TestRefusedBeatKeepsTheStampItWasLastGiven(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	previous := now.Add(3 * time.Second)
	got := dueStamp(previous, boardclient.HolderLiveness{}, false)
	if !got.Equal(previous) {
		t.Fatalf("refused beat moved the stamp to %v, want %v kept", got, previous)
	}
}

// A blip must not leave the loop waiting a bare interval against a due time
// that has since come closer. This walks the two together the way KeepAlive
// does: one answered beat, then two refusals, with the clock moving on.
func TestABlipWaitsAgainstTheStampItWasLastGiven(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	interval := 2 * time.Second
	stamp := dueStamp(time.Time{}, dueAt(now, 6*time.Second), true)

	if got := nextBeatWait(interval, stamp, now); got != interval {
		t.Fatalf("first wait = %v, want the cadence %v", got, interval)
	}
	now = now.Add(interval)
	stamp = dueStamp(stamp, boardclient.HolderLiveness{}, false)
	if got := nextBeatWait(interval, stamp, now); got != interval {
		t.Fatalf("wait after one blip = %v, want the cadence %v", got, interval)
	}
	now = now.Add(interval)
	stamp = dueStamp(stamp, boardclient.HolderLiveness{}, false)
	got := nextBeatWait(interval, stamp, now)
	if got != interval {
		t.Fatalf("wait after two blips = %v, want the cadence %v", got, interval)
	}
	now = now.Add(time.Second)
	if got := nextBeatWait(interval, stamp, now); got != time.Second {
		t.Fatalf("wait once the stamp is inside the cadence = %v, want 1s", got)
	}
	now = now.Add(900 * time.Millisecond)
	if got := nextBeatWait(interval, stamp, now); got != minHeartbeatInterval {
		t.Fatalf("wait once the stamp is inside the floor = %v, want %v", got, minHeartbeatInterval)
	}
}

// The first beat of a loop starts from defaultHeartbeatInterval, a number the
// server never said. Once a beat has answered, the due stamp it carried is
// what the next wait is held to, not that minute.
func TestTheOpeningMinuteYieldsToTheFirstStampTheServerGives(t *testing.T) {
	now := time.Date(2026, 9, 27, 4, 0, 0, 0, time.UTC)
	stamp := dueStamp(time.Time{}, dueAt(now, 2*time.Second), true)
	if got := nextBeatWait(defaultHeartbeatInterval, stamp, now); got != 2*time.Second {
		t.Fatalf("wait after the first answered beat = %v, want 2s", got)
	}
}
