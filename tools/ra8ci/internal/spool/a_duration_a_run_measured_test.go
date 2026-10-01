// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

func lasted(duration time.Duration, span time.Duration) executor.Result {
	started := time.Date(2026, 9, 27, 10, 0, 0, 0, time.UTC)
	return executor.Result{TaskName: "format-check", StartedAt: started,
		EndedAt: started.Add(span), Duration: duration}
}

func TestADurationLongerThanItsWindowIsRefused(t *testing.T) {
	if err := checkTheDurationWasMeasured(lasted(time.Hour, time.Millisecond)); !errors.Is(err, errUnmeasuredDuration) {
		t.Fatalf("an hour between stamps a millisecond apart was frozen: %v", err)
	}
}

func TestANegativeDurationIsRefused(t *testing.T) {
	if err := checkTheDurationWasMeasured(lasted(-time.Second, time.Minute)); !errors.Is(err, errUnmeasuredDuration) {
		t.Fatalf("a negative duration was frozen: %v", err)
	}
}

// Shorter than the window is ordinary: the stamps bracket the attempt, the
// duration may measure the child alone.
func TestADurationShorterThanItsWindowIsFrozen(t *testing.T) {
	if err := checkTheDurationWasMeasured(lasted(time.Second, time.Minute)); err != nil {
		t.Fatalf("a duration shorter than its window was refused: %v", err)
	}
}

func TestTheClockAllowanceIsFiveSeconds(t *testing.T) {
	if err := checkTheDurationWasMeasured(lasted(5*time.Second, 0)); err != nil {
		t.Fatalf("a duration inside the clock allowance was refused: %v", err)
	}
	if err := checkTheDurationWasMeasured(lasted(5*time.Second+time.Nanosecond, 0)); !errors.Is(err, errUnmeasuredDuration) {
		t.Fatalf("a duration past the clock allowance was frozen: %v", err)
	}
}

// An attempt that came apart before the executor could measure it states no
// stamps, and that shape is how the failure reaches history. The door judges a
// contradiction, not an omission.
func TestAnAttemptStatingNoStampsIsStillFrozen(t *testing.T) {
	if err := checkTheDurationWasMeasured(executor.Result{TaskName: "format-check", Duration: time.Hour}); err != nil {
		t.Fatalf("a result stating no stamps was refused: %v", err)
	}
	if err := checkTheDurationWasMeasured(executor.Result{}); err != nil {
		t.Fatalf("a result stating nothing at all was refused: %v", err)
	}
}

// The door reads the record alone. This host's clock is the thing under
// suspicion, so a window in the past or the future is not its business.
func TestTheDurationDoorDoesNotJudgeAgainstNow(t *testing.T) {
	future := executor.Result{TaskName: "format-check",
		StartedAt: time.Now().UTC().Add(72 * time.Hour),
		EndedAt:   time.Now().UTC().Add(73 * time.Hour), Duration: time.Hour}
	if err := checkTheDurationWasMeasured(future); err != nil {
		t.Fatalf("a window in the future was refused: %v", err)
	}
}

func TestFinishRefusesADurationNoRunMeasured(t *testing.T) {
	spool, entry := freezeForTest(t)
	if _, err := spool.Finish(entry, lasted(time.Hour, time.Millisecond), nil); !errors.Is(err, errUnmeasuredDuration) {
		t.Fatalf("an unmeasurable duration was written into a terminal record: %v", err)
	}
}
