// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"strings"
	"testing"
)

// The two guards that stand between a caller and a terminal attempt row.
// Neither reads the database, so both are judged here in full: FinishAttempt
// is exported and takes the struct, so a caller that builds one itself
// reaches these columns without passing any HTTP door first.

func finishedAttempt(t *testing.T) FinishAttemptInput {
	t.Helper()
	id, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	exit := 0
	return FinishAttemptInput{AttemptID: id, ActorID: "tester", Result: "succeeded",
		ChildExitCode: &exit, EvidenceComplete: true}
}

// The step states a row may take, and nothing beside them. "running" is the
// one worth naming: it is a real state a step passes through and still not a
// terminal one, so a caller writing it here would be closing a step that
// never ended.
func TestAStepStateIsOneOfFive(t *testing.T) {
	for _, state := range []string{"succeeded", "failed", "timed_out", "cancelled", "skipped"} {
		if !validStepState(state) {
			t.Errorf("%q refused", state)
		}
	}
	for _, state := range []string{"", "running", "pending", "scheduled", "preempted", "lost",
		"Succeeded", "succeeded ", "succeeded\n", "unknown"} {
		if validStepState(state) {
			t.Errorf("%q accepted", state)
		}
	}
}

// Who and what the attempt names, judged before its outcome is read at all.
func TestATerminalAttemptNamesItsAttemptAndActor(t *testing.T) {
	for name, spoil := range map[string]func(*FinishAttemptInput){
		"no attempt ID":                       func(in *FinishAttemptInput) { in.AttemptID = "" },
		"an ID that is not one":               func(in *FinishAttemptInput) { in.AttemptID = "attempt-17" },
		"no actor":                            func(in *FinishAttemptInput) { in.ActorID = "" },
		"a reason past the bound":             func(in *FinishAttemptInput) { in.Reason = strings.Repeat("x", 1025) },
		"no result":                           func(in *FinishAttemptInput) { in.Result = "" },
		"a result nothing reads":              func(in *FinishAttemptInput) { in.Result = "done" },
		"a step state, not an attempt result": func(in *FinishAttemptInput) { in.Result = "skipped" },
	} {
		in := finishedAttempt(t)
		spoil(&in)
		if validAttemptResult(in) {
			t.Errorf("%s accepted", name)
		}
	}

	at := finishedAttempt(t)
	at.Reason = strings.Repeat("x", 1024)
	if !validAttemptResult(at) {
		t.Error("a reason exactly at the bound was refused")
	}
}

// A success is the strictest outcome: a child that reported zero, no
// deadline, and evidence complete. Anything less is a green nobody can
// check, which is the one thing a run summary must not carry.
func TestASuccessIsTheStrictestOutcome(t *testing.T) {
	nonZero := 1
	for name, spoil := range map[string]func(*FinishAttemptInput){
		"no child exit code at all": func(in *FinishAttemptInput) { in.ChildExitCode = nil },
		"a child that reported 1":   func(in *FinishAttemptInput) { in.ChildExitCode = &nonZero },
		"a deadline it hit":         func(in *FinishAttemptInput) { in.HitDeadline = true },
		"evidence it never closed":  func(in *FinishAttemptInput) { in.EvidenceComplete = false },
	} {
		in := finishedAttempt(t)
		spoil(&in)
		if validAttemptResult(in) {
			t.Errorf("a success with %s was accepted", name)
		}
	}
}

// The deadline is what tells a timeout apart from every other ending, so it
// is required on one result and refused on the rest. A failure marked as
// having hit the deadline is a timeout written under the wrong name, and the
// scheduler reads the two differently.
func TestTheDeadlineDecidesWhichEndingThisIs(t *testing.T) {
	for _, result := range []string{"failed", "cancelled", "preempted", "lost"} {
		in := finishedAttempt(t)
		in.Result = result
		in.ChildExitCode = nil
		in.EvidenceComplete = false
		if !validAttemptResult(in) {
			t.Errorf("%q without a deadline was refused", result)
		}
		in.HitDeadline = true
		if validAttemptResult(in) {
			t.Errorf("%q was accepted while claiming the deadline", result)
		}
	}

	timedOut := finishedAttempt(t)
	timedOut.Result = "timed_out"
	timedOut.ChildExitCode = nil
	timedOut.EvidenceComplete = false
	if validAttemptResult(timedOut) {
		t.Error("a timeout that never hit the deadline was accepted")
	}
	timedOut.HitDeadline = true
	if !validAttemptResult(timedOut) {
		t.Error("a timeout that hit the deadline was refused")
	}
}

// An unevidenced success fails the task even though the attempt itself is
// recorded as it happened, and every other outcome travels unchanged. That
// asymmetry is the whole rule: the attempt row keeps the truth, the task
// result keeps the judgement.
func TestAnUnverifiableGreenIsNotOneOnTheTask(t *testing.T) {
	if got := taskResultFor("succeeded", false); got != "failed" {
		t.Fatalf("an unevidenced success became %q", got)
	}
	if got := taskResultFor("succeeded", true); got != "succeeded" {
		t.Fatalf("an evidenced success became %q", got)
	}
	for _, result := range []string{"failed", "timed_out", "cancelled", "preempted", "lost"} {
		for _, evidence := range []bool{false, true} {
			if got := taskResultFor(result, evidence); got != result {
				t.Errorf("%q with evidence=%v became %q", result, evidence, got)
			}
		}
	}
}
