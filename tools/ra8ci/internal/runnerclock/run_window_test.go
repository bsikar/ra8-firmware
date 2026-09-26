// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"strings"
	"testing"
	"time"
)

func mustStamp(t *testing.T, value string) time.Time {
	t.Helper()
	parsed, ok := parseTimestamp(value)
	if !ok {
		t.Fatalf("fixture timestamp %q does not parse", value)
	}
	return parsed
}

// offsetJob is a job whose steps are all shifted by one amount: internally
// ordered, collectively wrong.
func offsetJob(base string, offset time.Duration) job {
	start, ok := parseTimestamp(base)
	if !ok {
		panic("bad fixture base")
	}
	start = start.Add(offset)
	stamp := func(d time.Duration) string {
		return start.Add(d).UTC().Format(time.RFC3339)
	}
	return job{Name: "build", RunnerName: "win-ci-3", Steps: []step{
		{Name: "Set up job", StartedAt: stamp(0), CompletedAt: stamp(2 * time.Second)},
		{Name: "checkout", StartedAt: stamp(2 * time.Second), CompletedAt: stamp(9 * time.Second)},
		{Name: "gate", StartedAt: stamp(9 * time.Second), CompletedAt: stamp(40 * time.Second)},
	}}
}

func TestAStepCannotBeginBeforeTheRunThatDispatchedIt(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	findings := jobBeganWithinItsRun(offsetJob("2026-07-28T06:00:10Z", -time.Hour), dispatched, true)
	if len(findings) != 1 {
		t.Fatalf("findings = %d, want 1", len(findings))
	}
	if findings[0].kind != "began-before-the-run-that-dispatched-it" {
		t.Fatalf("kind = %q", findings[0].kind)
	}
	if findings[0].step != "Set up job" {
		t.Fatalf("step = %q, want the first step that ran", findings[0].step)
	}
	if findings[0].seconds >= 0 {
		t.Fatalf("seconds = %v, want the offset reported as negative like the other findings", findings[0].seconds)
	}
	if !strings.Contains(findings[0].detail, "run began 2026-07-28T06:00:00Z") {
		t.Fatalf("detail does not name the run's own clock: %q", findings[0].detail)
	}
}

// The whole point of the check: the offset job is clean by every rule the
// scanner had before it, so nothing else in this package disagrees with it.
func TestAConstantOffsetIsInvisibleToTheStepOrderingRules(t *testing.T) {
	skewed := offsetJob("2026-07-28T06:00:10Z", -time.Hour)
	if got := len(scanJob(skewed)); got != 0 {
		t.Fatalf("scanJob findings = %d, want 0; the fixture must be internally ordered", got)
	}
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	if got := len(jobBeganWithinItsRun(skewed, dispatched, true)); got != 1 {
		t.Fatalf("run-window findings = %d, want 1", got)
	}
}

func TestQueueingIsNotSkew(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	tests := []struct {
		name   string
		offset time.Duration
		want   int
	}{
		{"began with the run", 0, 0},
		{"queued four minutes", 4 * time.Minute, 0},
		{"queued two hours behind a concurrency group", 2 * time.Hour, 0},
		{"four seconds early is inside the tolerance", -4 * time.Second, 0},
		{"five seconds early is inside the tolerance", -5 * time.Second, 0},
		{"six seconds early is a finding", -6 * time.Second, 1},
		{"an hour early is a finding", -time.Hour, 1},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			subject := offsetJob("2026-07-28T06:00:00Z", test.offset)
			if got := len(jobBeganWithinItsRun(subject, dispatched, true)); got != test.want {
				t.Fatalf("findings = %d, want %d", got, test.want)
			}
		})
	}
}

func TestTheTwoRulesShareOneTolerance(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	for offset := -overlapTolerance - 3*time.Second; offset <= time.Second; offset += time.Second {
		early := -offset
		wantFinding := early > overlapTolerance
		got := len(jobBeganWithinItsRun(offsetJob("2026-07-28T06:00:00Z", offset), dispatched, true)) == 1
		if got != wantFinding {
			t.Fatalf("offset %v: finding = %t, want %t; the run-window rule must use the same tolerance the overlap rule does", offset, got, wantFinding)
		}
	}
}

func TestOneFindingPerJobHoweverManyStepsCarryTheOffset(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	skewed := offsetJob("2026-07-28T06:00:00Z", -time.Hour)
	if len(skewed.Steps) != 3 {
		t.Fatalf("fixture has %d steps, want 3", len(skewed.Steps))
	}
	if got := len(jobBeganWithinItsRun(skewed, dispatched, true)); got != 1 {
		t.Fatalf("findings = %d, want 1 however many steps carry the same offset", got)
	}
}

func TestNothingIsJudgedWithoutTheRunsOwnClock(t *testing.T) {
	skewed := offsetJob("2026-07-28T06:00:00Z", -time.Hour)
	if got := jobBeganWithinItsRun(skewed, time.Time{}, false); got != nil {
		t.Fatalf("findings = %v, want none when the run does not state when it began", got)
	}
}

func TestAStepWithNoStartHasNothingToJudge(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	subject := job{Steps: []step{
		{Name: "skipped"},
		{Name: "malformed", StartedAt: "not a timestamp"},
	}}
	if got := len(jobBeganWithinItsRun(subject, dispatched, true)); got != 0 {
		t.Fatalf("findings = %d, want 0", got)
	}
}

func TestAnUnnamedStepIsStillReported(t *testing.T) {
	dispatched := mustStamp(t, "2026-07-28T06:00:00Z")
	subject := job{Steps: []step{{StartedAt: "2026-07-28T05:00:00Z", CompletedAt: "2026-07-28T05:00:01Z"}}}
	findings := jobBeganWithinItsRun(subject, dispatched, true)
	if len(findings) != 1 || findings[0].step != "?" {
		t.Fatalf("findings = %+v, want one named '?'", findings)
	}
}

func TestRunStartPrefersTheDispatchStampAndFallsBack(t *testing.T) {
	tests := []struct {
		name    string
		subject run
		want    string
		ok      bool
	}{
		{"the dispatch stamp", run{RunStarted: "2026-07-28T06:00:00Z", CreatedAt: "2026-07-28T05:00:00Z"}, "2026-07-28T06:00:00Z", true},
		{"created_at when the run predates the field", run{CreatedAt: "2026-07-28T05:00:00Z"}, "2026-07-28T05:00:00Z", true},
		{"created_at when the dispatch stamp is malformed", run{RunStarted: "nonsense", CreatedAt: "2026-07-28T05:00:00Z"}, "2026-07-28T05:00:00Z", true},
		{"neither", run{}, "", false},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, ok := runStart(test.subject)
			if ok != test.ok {
				t.Fatalf("ok = %t, want %t", ok, test.ok)
			}
			if ok && got.UTC().Format(time.RFC3339) != test.want {
				t.Fatalf("runStart = %s, want %s", got.UTC().Format(time.RFC3339), test.want)
			}
		})
	}
}
