// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// terminalReceipt has two corrections that fire after the outcome is chosen and
// after the times are gathered. Both exist because the values they correct
// would otherwise be refused at the door or believed wrongly, and neither is
// reachable from an ordinary attempt, which is what leaves them unpinned.

// An attempt that ran no step at all has nothing to have succeeded at. The
// outcome is chosen before the step count is consulted, so without this
// correction a task whose steps were all skipped would be filed green with an
// empty step list, which is the one shape an operator cannot tell from a real
// pass by reading the outcome alone.
func TestAnAttemptThatRanNoStepIsNotASuccess(t *testing.T) {
	start := time.Now().Add(-time.Minute)
	receipt := terminalReceipt(testAssignment(), executor.Result{
		StartedAt: start, EndedAt: start.Add(time.Second), Duration: time.Second,
	}, runnerFacts(start), runnerFacts(start.Add(time.Minute)), 0, nil, nil, nil)

	if receipt.Outcome != "failed" {
		t.Fatalf("outcome = %q, want failed: an attempt with no steps has not succeeded", receipt.Outcome)
	}
	if receipt.EvidenceComplete {
		t.Fatal("an attempt with no steps was filed with complete evidence")
	}
	if receipt.ErrorCode != "no_step_executed" {
		t.Fatalf("error code = %q, want no_step_executed", receipt.ErrorCode)
	}
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the corrected receipt is not one the plane would accept: %v", err)
	}
}

// The same result carrying one step is the control: it keeps succeeded, so the
// case above is pinned on the step count and not on something else about the
// fixture.
func TestTheSameAttemptWithAStepIsASuccess(t *testing.T) {
	start := time.Now().Add(-time.Minute)
	receipt := terminalReceipt(testAssignment(), executor.Result{
		StartedAt: start, EndedAt: start.Add(time.Second), Duration: time.Second,
		Steps: []executor.StepResult{{Name: "format_tree", StartedAt: start,
			EndedAt: start.Add(time.Second), Duration: time.Second}},
	}, runnerFacts(start), runnerFacts(start.Add(time.Minute)), 1, nil, nil, nil)

	if receipt.Outcome != "succeeded" || !receipt.EvidenceComplete || receipt.ErrorCode != "" {
		t.Fatalf("outcome = %q evidence = %v code = %q, want a clean succeeded receipt",
			receipt.Outcome, receipt.EvidenceComplete, receipt.ErrorCode)
	}
}

// An end that precedes its own start is refused by Validate, so a receipt
// carrying one would be built and then thrown away at the door, losing the
// whole report of an attempt that did run. The times are clamped instead.
//
// Reaching it takes a result that recorded when it ended but not when it
// started, so the start host reading stands in for the start and an older end
// stamp then dates the attempt as finishing before it began. The two host
// readings stay in order and still bracket the attempt, which are separate
// rules and different defects.
func TestAnEndBeforeItsStartIsClampedRatherThanRefused(t *testing.T) {
	read := time.Date(2026, 9, 26, 15, 0, 0, 0, time.UTC)
	receipt := terminalReceipt(testAssignment(), executor.Result{
		EndedAt: read.Add(-5 * time.Second),
	}, runnerFacts(read), runnerFacts(read.Add(5*time.Second)), 1, nil, nil, nil)

	if receipt.EndedAt.Before(receipt.StartedAt) {
		t.Fatalf("ended %v precedes started %v: Validate would refuse this receipt",
			receipt.EndedAt, receipt.StartedAt)
	}
	if !receipt.EndedAt.Equal(receipt.StartedAt) {
		t.Fatalf("ended = %v, want it clamped to started %v rather than moved elsewhere",
			receipt.EndedAt, receipt.StartedAt)
	}
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the clamped receipt is not one the plane would accept: %v", err)
	}
}

// The clamp must not touch an ordering that was already honest, or every
// receipt would report a zero-length attempt.
func TestAnEndAfterItsStartIsLeftAlone(t *testing.T) {
	start := time.Now().Add(-time.Minute)
	ended := start.Add(42 * time.Second)
	receipt := terminalReceipt(testAssignment(), executor.Result{
		StartedAt: start, EndedAt: ended, Duration: 42 * time.Second,
		Steps: []executor.StepResult{{Name: "format_tree"}},
	}, runnerFacts(start), runnerFacts(time.Now()), 1, nil, nil, nil)

	if !receipt.StartedAt.Equal(start) || !receipt.EndedAt.Equal(ended) {
		t.Fatalf("started = %v ended = %v, want the result's own times untouched (%v, %v)",
			receipt.StartedAt, receipt.EndedAt, start, ended)
	}
}
