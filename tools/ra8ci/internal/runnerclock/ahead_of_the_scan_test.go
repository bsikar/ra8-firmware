// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"strings"
	"testing"
	"time"
)

// scanAt is the moment every test below pretends the scan read GitHub.
var scanAt = time.Date(2026, 7, 28, 12, 0, 0, 0, time.UTC)

func stamp(offset time.Duration) string {
	return scanAt.Add(offset).UTC().Format(time.RFC3339)
}

func aheadFindings(t *testing.T, steps ...step) []finding {
	t.Helper()
	return stampedAheadOfTheScan(job{Steps: steps}, scanAt)
}

func TestAStepStampedAfterTheScanIsAFinding(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "gate", StartedAt: stamp(time.Hour), CompletedAt: stamp(time.Hour + time.Minute)})
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1", len(found))
	}
	if found[0].kind != "stamped-after-the-scan-that-read-it" {
		t.Fatalf("kind = %q", found[0].kind)
	}
	if found[0].step != "gate" {
		t.Fatalf("step = %q, want gate", found[0].step)
	}
	if found[0].seconds <= 0 {
		t.Fatalf("seconds = %v, want the distance ahead as a positive number", found[0].seconds)
	}
}

func TestAJobInTheScansPastIsClean(t *testing.T) {
	found := aheadFindings(t,
		step{Number: 1, Name: "set up", StartedAt: stamp(-48 * time.Hour), CompletedAt: stamp(-48*time.Hour + 4*time.Second)},
		step{Number: 2, Name: "checkout", StartedAt: stamp(-48*time.Hour + 4*time.Second), CompletedAt: stamp(-48*time.Hour + 9*time.Second)},
	)
	if len(found) != 0 {
		t.Fatalf("findings = %d, want 0: a run read after it ran is the ordinary case", len(found))
	}
}

func TestSkewInsideTheToleranceIsNotAFault(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "gate", StartedAt: stamp(-time.Minute), CompletedAt: stamp(scanSkewTolerance - 30*time.Second)})
	if len(found) != 0 {
		t.Fatalf("findings = %d, want 0: a host a little off its own time is not the fault this looks for", len(found))
	}
}

func TestJustPastTheToleranceIsAFault(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "gate", StartedAt: stamp(-time.Minute), CompletedAt: stamp(scanSkewTolerance + time.Second)})
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1", len(found))
	}
}

func TestOneFindingPerJobHoweverManyStepsAreAhead(t *testing.T) {
	var steps []step
	for i := 1; i <= 20; i++ {
		steps = append(steps, step{Number: i, Name: "step", StartedAt: stamp(time.Hour), CompletedAt: stamp(time.Hour + time.Second)})
	}
	found := aheadFindings(t, steps...)
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1: one host's constant offset must not bury the rest of the report", len(found))
	}
}

func TestTheFindingNamesTheStepTheRunnerRanFirst(t *testing.T) {
	found := aheadFindings(t,
		step{Number: 2, Name: "second", StartedAt: stamp(time.Hour + time.Minute), CompletedAt: stamp(time.Hour + 2*time.Minute)},
		step{Number: 1, Name: "first", StartedAt: stamp(time.Hour), CompletedAt: stamp(time.Hour + time.Minute)},
	)
	if len(found) != 1 || found[0].step != "first" {
		t.Fatalf("findings = %+v, want the step numbered 1", found)
	}
}

func TestTheCallersStepsAreNotReordered(t *testing.T) {
	steps := []step{
		{Number: 2, Name: "second", StartedAt: stamp(time.Hour + time.Minute)},
		{Number: 1, Name: "first", StartedAt: stamp(time.Hour)},
	}
	aheadFindings(t, steps...)
	if steps[0].Name != "second" || steps[1].Name != "first" {
		t.Fatalf("the caller's slice was reordered: %+v", steps)
	}
}

func TestAStepWithNoTimestampsIsNotEvidence(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "never ran"})
	if len(found) != 0 {
		t.Fatalf("findings = %d, want 0: a skipped step says nothing about any clock", len(found))
	}
}

func TestAStepWithOnlyAStartIsStillJudged(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "still running", StartedAt: stamp(90 * time.Minute)})
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1", len(found))
	}
}

func TestAnUnreadableTimestampIsSkippedNotJudged(t *testing.T) {
	found := aheadFindings(t,
		step{Number: 1, Name: "garbled", StartedAt: "not a timestamp", CompletedAt: "also not one"},
		step{Number: 2, Name: "ahead", StartedAt: stamp(time.Hour), CompletedAt: stamp(time.Hour + time.Second)},
	)
	if len(found) != 1 || found[0].step != "ahead" {
		t.Fatalf("findings = %+v, want only the step with readable stamps", found)
	}
}

func TestABrokenPairIsJudgedOnItsLaterStamp(t *testing.T) {
	// completed_at before started_at is a finding scanJob already reports;
	// reading completed_at blindly here would hide the start stamped a day
	// ahead behind it.
	found := aheadFindings(t, step{Number: 1, Name: "gate", StartedAt: stamp(24 * time.Hour), CompletedAt: stamp(-24 * time.Hour)})
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1", len(found))
	}
	if !strings.Contains(found[0].detail, stamp(24*time.Hour)) {
		t.Fatalf("detail = %q, want the later stamp named", found[0].detail)
	}
}

func TestAJobWithNoStepsIsClean(t *testing.T) {
	if found := stampedAheadOfTheScan(job{}, scanAt); len(found) != 0 {
		t.Fatalf("findings = %d, want 0", len(found))
	}
}

func TestNoScanClockJudgesNothing(t *testing.T) {
	found := stampedAheadOfTheScan(job{Steps: []step{{Number: 1, Name: "gate", StartedAt: stamp(72 * time.Hour)}}}, time.Time{})
	if len(found) != 0 {
		t.Fatalf("findings = %d, want 0: with no clock of its own the scan has nothing to hold a stamp against", len(found))
	}
}

func TestTheDetailNamesBothClocks(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, Name: "gate", StartedAt: stamp(time.Hour), CompletedAt: stamp(time.Hour + time.Minute)})
	if len(found) != 1 {
		t.Fatalf("findings = %d, want 1", len(found))
	}
	detail := found[0].detail
	if !strings.Contains(detail, scanAt.Format(time.RFC3339)) || !strings.Contains(detail, stamp(time.Hour+time.Minute)) {
		t.Fatalf("detail = %q, want the scan's moment and the step's stamp", detail)
	}
	if !found[0].at.Equal(scanAt.Add(time.Hour + time.Minute)) {
		t.Fatalf("at = %v, want the offending stamp", found[0].at)
	}
}

func TestAnUnnamedStepAheadOfTheScanIsStillReported(t *testing.T) {
	found := aheadFindings(t, step{Number: 1, StartedAt: stamp(time.Hour)})
	if len(found) != 1 || found[0].step != "?" {
		t.Fatalf("findings = %+v, want one finding naming an unnamed step", found)
	}
}
