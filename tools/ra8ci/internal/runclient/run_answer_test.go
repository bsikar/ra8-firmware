// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runclient

import (
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const answeredRunID = "00000000-0000-7000-8000-000000000001"

func answered(id, state string) store.Run {
	return store.Run{ID: id, State: state}
}

func TestEveryStateTheRunMachineKnowsIsAnsweredWith(t *testing.T) {
	for _, state := range []string{"queued", "running", "terminal"} {
		if err := checkAnsweredRun(answered(answeredRunID, state), answeredRunID); err != nil {
			t.Fatalf("state %q refused: %v", state, err)
		}
	}
}

func TestAnAnsweredStateNoRunCanHoldIsRefused(t *testing.T) {
	for _, state := range []string{"", "Queued", "succeeded", "cancelled", "lost", "unknown", "pending"} {
		if err := checkAnsweredRun(answered(answeredRunID, state), answeredRunID); err == nil {
			t.Fatalf("state %q accepted", state)
		}
	}
}

func TestTheStateOfARunIsJudgedAgainstTheRunMachineNotTheTaskMachine(t *testing.T) {
	// "succeeded" and "failed" are states of tasks and attempts, and reading
	// either as a run state is the mistake a hand-written list invites.
	for _, state := range []string{"succeeded", "failed", "timed_out", "preempted", "skipped"} {
		if store.KnownRunState(state) {
			t.Fatalf("%q is a run state after all; this test is wrong", state)
		}
		if err := checkAnsweredRun(answered(answeredRunID, state), answeredRunID); err == nil {
			t.Fatalf("task state %q accepted as a run state", state)
		}
	}
}

func TestARunOtherThanTheOneRequestedIsRefusedWhateverItsState(t *testing.T) {
	const other = "00000000-0000-7000-8000-000000000002"
	if err := checkAnsweredRun(answered(other, "running"), answeredRunID); err == nil {
		t.Fatal("foreign run accepted")
	}
}

func TestAMalformedIdentifierIsRefusedEvenWhenItMatches(t *testing.T) {
	if err := checkAnsweredRun(answered("not-a-run", "running"), "not-a-run"); err == nil {
		t.Fatal("malformed identifier accepted")
	}
}

func TestAReadNamesTheIdentifierBeforeTheState(t *testing.T) {
	// Both halves are wrong at once. The identifier is the more useful thing
	// to name, because it says the answer was about something else entirely.
	err := checkAnsweredRun(answered("00000000-0000-7000-8000-000000000002", "nonsense"), answeredRunID)
	if err == nil || !strings.Contains(err.Error(), "not the run requested") {
		t.Fatalf("error = %v", err)
	}
}

func TestAnAbsurdAnsweredStateIsNamedButBounded(t *testing.T) {
	err := checkAnsweredRun(answered(answeredRunID, strings.Repeat("s", 4096)), answeredRunID)
	if err == nil {
		t.Fatal("absurd state accepted")
	}
	if len(err.Error()) > 200 || !strings.Contains(err.Error(), "...") {
		t.Fatalf("error not bounded: %d chars", len(err.Error()))
	}
}

func TestAStateAtTheReportingBoundaryIsNotTruncated(t *testing.T) {
	state := strings.Repeat("s", 64)
	err := checkAnsweredRun(answered(answeredRunID, state), answeredRunID)
	if err == nil || !strings.Contains(err.Error(), state) || strings.Contains(err.Error(), "...") {
		t.Fatalf("error = %v", err)
	}
}

func TestTheOtherFieldsOfARunAreNotJudged(t *testing.T) {
	// A client that refused a run for how its stamps read would be refusing
	// runs that exist. Only identity and the state vocabulary are held here.
	stamp := time.Date(2026, 9, 22, 0, 0, 0, 0, time.UTC)
	run := store.Run{ID: answeredRunID, State: "terminal", StartedAt: nil, EndedAt: nil,
		CancelRequestedAt: &stamp, ExecutionResult: "", CleanupResult: "", EvidenceState: ""}
	if err := checkAnsweredRun(run, answeredRunID); err != nil {
		t.Fatalf("run refused on fields it does not judge: %v", err)
	}
}

func TestARunClientJudgesASubmissionAndAReadByTheSameVocabulary(t *testing.T) {
	// checkSubmitReceipt and checkAnsweredRun are the two places a state
	// string reaches a caller. They agree, or a run readable through one door
	// is unreadable through the other.
	for _, state := range []string{"queued", "running", "terminal", "succeeded", "", "nonsense"} {
		receipt := checkSubmitReceipt(Receipt{ID: answeredRunID, State: state}) == nil
		read := checkAnsweredRun(answered(answeredRunID, state), answeredRunID) == nil
		if receipt != read {
			t.Fatalf("state %q: submission accepts %v, read accepts %v", state, receipt, read)
		}
	}
}

func TestBoundedForErrorLeavesShortValuesAlone(t *testing.T) {
	for _, value := range []string{"", "queued", strings.Repeat("x", 63), strings.Repeat("x", 64)} {
		if boundedForError(value) != value {
			t.Fatalf("value of %d chars was cut", len(value))
		}
	}
	if cut := boundedForError(strings.Repeat("x", 65)); cut != strings.Repeat("x", 64)+"..." {
		t.Fatalf("cut = %q", cut)
	}
}
