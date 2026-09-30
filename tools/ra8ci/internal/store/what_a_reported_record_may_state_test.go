// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// Two refusals an agent-reported record meets before it is written: a receipt
// claiming more steps than the assignment has, and a run naming an outcome the
// store has no column for.
//
// Both are the arms the existing tables skip. The step-evidence table judges
// receipts that fit the assignment, so the bound itself goes unexercised; the
// local-run tables vary one field of a valid record at a time, and Result is
// the one field whose refusal is a switch default rather than part of the big
// conjunction.

// TestAReceiptCannotClaimMoreStepsThanTheAssignmentHas pins the bound at the
// head of the check. Everything below it reasons about a PREFIX of the
// assignment's steps: which step is last, whether it explains the stop. A
// receipt with more steps than the assignment declared is not a prefix of
// anything, so the questions below are not meaningful about it, and admitting
// it would write step rows against ordinals the assignment never had.
func TestAReceiptCannotClaimMoreStepsThanTheAssignmentHas(t *testing.T) {
	three := protocol.TerminalReceipt{EvidenceComplete: true,
		Steps: []protocol.StepSummary{{}, {}, {}}}

	if validPartialStepEvidence(three, 2) {
		t.Fatal("a receipt claiming three steps was accepted for a two-step assignment")
	}
	// One step for an assignment that declares none is the same overrun at
	// the smallest size.
	if validPartialStepEvidence(protocol.TerminalReceipt{Steps: []protocol.StepSummary{{}}}, 0) {
		t.Fatal("a step was accepted for an assignment with none")
	}
	// A negative count is not a small assignment. It is a caller defect,
	// and the empty receipt that would otherwise pass as a complete run of
	// no steps is the shape that hides it.
	if validPartialStepEvidence(protocol.TerminalReceipt{}, -1) {
		t.Fatal("a negative step count was treated as an assignment")
	}
	// The same receipt over an assignment that does declare three steps is
	// taken, so the refusal above is the bound and not the receipt.
	if !validPartialStepEvidence(three, 3) {
		t.Fatal("a complete three-step receipt was refused")
	}
}

// TestALocalRunNamesAnOutcomeTheStoreKnows pins the result switch's default.
// Result decides how every later read treats the record: which runs count as
// failures, which are excluded as cancelled, which are incomplete evidence
// rather than a verdict. An unknown word would be stored and then silently
// match none of those, so the record would exist and never be counted.
func TestALocalRunNamesAnOutcomeTheStoreKnows(t *testing.T) {
	for _, result := range []string{"", "success", "SUCCEEDED", "errored", "passed", "skipped", "unknown"} {
		run := localRun()
		run.Result = result
		err := validateLocalRun(run)
		if err == nil {
			t.Fatalf("result %q was accepted", result)
		}
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("the refusal of %q does not travel as invalid: %v", result, err)
		}
	}

	// Every word the store does know is taken. The four beside "succeeded"
	// state a child that did not finish cleanly, so they carry an exit code
	// and an executor error the success arm would refuse.
	if err := validateLocalRun(localRun()); err != nil {
		t.Fatalf("a succeeded run was refused: %v", err)
	}
	for _, result := range []string{"failed", "timed_out", "cancelled", "incomplete_evidence"} {
		run := localRun()
		run.Result, run.ChildExitCode = result, 1
		run.Steps[0].ExitCode = 1
		if err := validateLocalRun(run); err != nil {
			t.Fatalf("result %q was refused: %v", result, err)
		}
	}
}
