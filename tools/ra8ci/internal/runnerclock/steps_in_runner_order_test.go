// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"encoding/json"
	"strings"
	"testing"
	"time"
)

// numbered builds a step carrying the runner's own step number.
func numbered(number int, name, started, completed string) step {
	return step{Number: number, Name: name, StartedAt: started, CompletedAt: completed}
}

func namesOf(steps []step) []string {
	names := make([]string, 0, len(steps))
	for _, item := range steps {
		names = append(names, item.Name)
	}
	return names
}

func sameNames(got []step, want ...string) bool {
	names := namesOf(got)
	if len(names) != len(want) {
		return false
	}
	for i := range names {
		if names[i] != want[i] {
			return false
		}
	}
	return true
}

func TestNumberedStepsComeBackInTheRunnersOwnOrder(t *testing.T) {
	arrived := []step{
		numbered(3, "gate", "2026-07-28T06:00:09Z", "2026-07-28T06:00:40Z"),
		numbered(1, "Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:00:02Z"),
		numbered(2, "checkout", "2026-07-28T06:00:02Z", "2026-07-28T06:00:09Z"),
	}
	got := stepsInRunnerOrder(arrived)
	if !sameNames(got, "Set up job", "checkout", "gate") {
		t.Fatalf("order = %v, want the runner's numbering", namesOf(got))
	}
}

func TestReorderingDoesNotDisturbTheCallersOwnList(t *testing.T) {
	arrived := []step{
		numbered(2, "checkout", "2026-07-28T06:00:02Z", "2026-07-28T06:00:09Z"),
		numbered(1, "Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:00:02Z"),
	}
	_ = stepsInRunnerOrder(arrived)
	if !sameNames(arrived, "checkout", "Set up job") {
		t.Fatalf("caller's slice was reordered in place: %v", namesOf(arrived))
	}
}

func TestAListWithoutNumbersIsHandedBackExactlyAsItArrived(t *testing.T) {
	arrived := []step{
		{Name: "b", StartedAt: "2026-07-28T06:00:05Z", CompletedAt: "2026-07-28T06:00:09Z"},
		{Name: "a", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:05Z"},
	}
	if !sameNames(stepsInRunnerOrder(arrived), "b", "a") {
		t.Fatalf("an unnumbered list must not be reordered on a guess")
	}
}

func TestHalfAnOrderingKeyLeavesTheWholeListAlone(t *testing.T) {
	cases := []struct {
		name  string
		steps []step
	}{
		{"one step carries no number", []step{
			numbered(2, "b", "", ""),
			{Name: "a"},
		}},
		{"a number is repeated", []step{
			numbered(1, "b", "", ""),
			numbered(1, "a", "", ""),
		}},
		{"a number is zero", []step{
			numbered(0, "b", "", ""),
			numbered(1, "a", "", ""),
		}},
		{"a number is negative", []step{
			numbered(-1, "b", "", ""),
			numbered(1, "a", "", ""),
		}},
	}
	for _, item := range cases {
		t.Run(item.name, func(t *testing.T) {
			if !sameNames(stepsInRunnerOrder(item.steps), "b", "a") {
				t.Fatalf("%s: the list was reordered on an unusable key", item.name)
			}
		})
	}
}

func TestEmptyAndSingleStepListsAreHandedStraightBack(t *testing.T) {
	if got := stepsInRunnerOrder(nil); got != nil {
		t.Fatalf("nil steps = %v, want nil", got)
	}
	one := []step{numbered(7, "only", "", "")}
	if !sameNames(stepsInRunnerOrder(one), "only") {
		t.Fatalf("a single step must come back untouched")
	}
}

// The false-SKEW direction: two ordinary steps, handed back in the wrong
// order, used to read as an overlap and exit 1 against a healthy runner.
func TestAReorderedCleanJobIsNotReportedAsSkew(t *testing.T) {
	clean := job{Name: "build", RunnerName: "win-ci-3", Steps: []step{
		numbered(2, "checkout", "2026-07-28T06:05:00Z", "2026-07-28T06:10:00Z"),
		numbered(1, "Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:05:00Z"),
	}}
	if findings := scanJob(clean); len(findings) != 0 {
		t.Fatalf("findings = %d (%q), want a time-ordered job to stay clean", len(findings), findings[0].detail)
	}
}

// The hidden-SKEW direction, and the worse one: a step that never finished
// carries no end for the next step to be judged against, so a genuine overlap
// behind it disappeared entirely when the list arrived out of order.
func TestAnOverlapHiddenByAReorderedListIsStillFound(t *testing.T) {
	skewed := job{Name: "build", RunnerName: "win-ci-3", Steps: []step{
		numbered(2, "cancelled", "2026-07-28T06:05:00Z", ""),
		numbered(1, "gate", "2026-07-28T06:00:00Z", "2026-07-28T06:10:00Z"),
	}}
	findings := scanJob(skewed)
	if len(findings) != 1 {
		t.Fatalf("findings = %d, want the overlap the runner's order shows", len(findings))
	}
	if findings[0].kind != "started-before-the-previous-step-finished" {
		t.Fatalf("kind = %q", findings[0].kind)
	}
	if !strings.Contains(findings[0].detail, "'gate' ended") || !strings.Contains(findings[0].detail, "'cancelled' began") {
		t.Fatalf("detail names the wrong pair: %q", findings[0].detail)
	}
}

// Every arrangement of one numbered job has to produce the same report, which
// is the whole property this rule buys.
func TestEveryArrangementOfOneJobScansTheSame(t *testing.T) {
	ordered := []step{
		numbered(1, "Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:00:02Z"),
		numbered(2, "checkout", "2026-07-28T06:00:02Z", "2026-07-28T06:00:09Z"),
		numbered(3, "gate", "2026-07-28T06:00:00Z", "2026-07-28T06:00:40Z"),
		numbered(4, "upload", "2026-07-28T06:00:40Z", "2026-07-28T06:00:44Z"),
	}
	want := scanJob(job{Steps: ordered})
	if len(want) != 1 {
		t.Fatalf("fixture findings = %d, want exactly the one planted overlap", len(want))
	}
	var walk func(remaining, chosen []step)
	arrangements := 0
	walk = func(remaining, chosen []step) {
		if len(remaining) == 0 {
			arrangements++
			got := scanJob(job{Steps: chosen})
			if len(got) != len(want) {
				t.Fatalf("arrangement %v: findings = %d, want %d", namesOf(chosen), len(got), len(want))
			}
			for i := range got {
				if got[i].kind != want[i].kind || got[i].step != want[i].step || got[i].detail != want[i].detail {
					t.Fatalf("arrangement %v: finding %d = %+v, want %+v", namesOf(chosen), i, got[i], want[i])
				}
			}
			return
		}
		for i := range remaining {
			next := make([]step, 0, len(remaining)-1)
			next = append(next, remaining[:i]...)
			next = append(next, remaining[i+1:]...)
			walk(next, append(append([]step{}, chosen...), remaining[i]))
		}
	}
	walk(ordered, nil)
	if arrangements != 24 {
		t.Fatalf("arrangements = %d, want every permutation of four steps", arrangements)
	}
}

// The run-window rule reports one finding per job, so which step it names
// depends on the order too.
func TestTheStepNamedAgainstTheRunIsTheOneThatRanFirst(t *testing.T) {
	dispatched, ok := parseTimestamp("2026-07-28T07:00:00Z")
	if !ok {
		t.Fatal("fixture timestamp does not parse")
	}
	behind := job{Name: "build", RunnerName: "win-ci-3", Steps: []step{
		numbered(2, "checkout", "2026-07-28T06:00:02Z", "2026-07-28T06:00:09Z"),
		numbered(1, "Set up job", "2026-07-28T06:00:00Z", "2026-07-28T06:00:02Z"),
	}}
	findings := jobBeganWithinItsRun(behind, dispatched, true)
	if len(findings) != 1 {
		t.Fatalf("findings = %d, want one per job", len(findings))
	}
	if findings[0].step != "Set up job" {
		t.Fatalf("step = %q, want the step the runner numbered first", findings[0].step)
	}
	if findings[0].seconds >= 0 {
		t.Fatalf("seconds = %v, want the offset reported as negative", findings[0].seconds)
	}
}

// The field has to survive decoding, since nothing above is reachable if the
// tag is wrong.
func TestTheRunnersStepNumberIsDecoded(t *testing.T) {
	var payload jobList
	body := `{"jobs":[{"name":"build","runner_name":"win-ci-3","steps":[
		{"name":"checkout","number":2,"started_at":"2026-07-28T06:00:02Z","completed_at":"2026-07-28T06:00:09Z"},
		{"name":"Set up job","number":1,"started_at":"2026-07-28T06:00:00Z","completed_at":"2026-07-28T06:00:02Z"}]}]}`
	if err := json.Unmarshal([]byte(body), &payload); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(payload.Jobs) != 1 || len(payload.Jobs[0].Steps) != 2 {
		t.Fatalf("payload = %+v", payload)
	}
	if payload.Jobs[0].Steps[0].Number != 2 || payload.Jobs[0].Steps[1].Number != 1 {
		t.Fatalf("step numbers = %d,%d, want 2,1 as sent", payload.Jobs[0].Steps[0].Number, payload.Jobs[0].Steps[1].Number)
	}
	if !sameNames(stepsInRunnerOrder(payload.Jobs[0].Steps), "Set up job", "checkout") {
		t.Fatalf("a decoded payload is not ordered by the numbers it carries")
	}
}

// A numbered job that is genuinely fine stays fine, whatever the clock does
// inside the tolerance the scanner already allows.
func TestNumberingDoesNotInventFindingsOnACleanJob(t *testing.T) {
	base, ok := parseTimestamp("2026-07-28T06:00:00Z")
	if !ok {
		t.Fatal("fixture timestamp does not parse")
	}
	stamp := func(d time.Duration) string { return base.Add(d).UTC().Format(time.RFC3339) }
	clean := job{Steps: []step{
		numbered(1, "Set up job", stamp(0), stamp(2*time.Second)),
		numbered(2, "checkout", stamp(time.Second), stamp(9*time.Second)),
		numbered(3, "gate", stamp(9*time.Second), stamp(40*time.Second)),
	}}
	if findings := scanJob(clean); len(findings) != 0 {
		t.Fatalf("findings = %d, want rounding inside the tolerance left alone", len(findings))
	}
}
