// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
)

// outcomeReceipt is a succeeded receipt whose one step reports the clean run
// that outcome claims, so each test below changes exactly the verdict it is
// about.
func outcomeReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	return evidenceReceipt(t)
}

func TestACleanStepUnderASucceededAttemptIsAccepted(t *testing.T) {
	if err := outcomeReceipt(t).Validate(); err != nil {
		t.Fatalf("a succeeded receipt carrying a clean step was refused: %v", err)
	}
}

func TestASucceededAttemptCarryingAFailedStepIsRefused(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Steps[0].ExitCode = 1
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a succeeded attempt carrying a failed step was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "exiting 1") {
		t.Fatalf("the refusal does not name the step's own verdict: %v", err)
	}
}

func TestAStepTimeoutTheAttemptDoesNotReportIsRefused(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.ChildExitCode, receipt.EvidenceComplete = "failed", nil, false
	receipt.Steps[0].TimedOut = true
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step timeout the attempt denies was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "timeout the attempt does not") {
		t.Fatalf("the refusal does not say the two halves disagree: %v", err)
	}
}

func TestAStepCancellationTheAttemptDoesNotReportIsRefused(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.ChildExitCode, receipt.EvidenceComplete = "failed", nil, false
	receipt.Steps[0].Cancelled = true
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step cancellation the attempt denies was admitted: %v", err)
	}
}

func TestATimedOutAttemptMayCarryTheStepThatTimedOut(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.TimedOut, receipt.ChildExitCode = "timed_out", true, nil
	receipt.EvidenceComplete = false
	receipt.Steps[0].TimedOut, receipt.Steps[0].ExitCode = true, -1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a timed out attempt was refused its own timed out step: %v", err)
	}
}

func TestACancelledAttemptMayCarryTheStepThatWasCancelled(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.Cancelled, receipt.ChildExitCode = "cancelled", true, nil
	receipt.EvidenceComplete = false
	receipt.Steps[0].Cancelled, receipt.Steps[0].ExitCode = true, -1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a cancelled attempt was refused its own cancelled step: %v", err)
	}
}

// An attempt can fail before or between its steps (a bind error, a failed log
// upload), and then every step it did run reports clean. That is honest.
func TestAFailedAttemptNeedNotCarryAFailedStep(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	receipt.ErrorCode = "log_upload_error"
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a failure between clean steps was refused: %v", err)
	}
}

func TestAFailedAttemptMayCarryTheStepThatFailed(t *testing.T) {
	receipt := outcomeReceipt(t)
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	one := 1
	receipt.ChildExitCode = &one
	receipt.Steps[0].ExitCode = 1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a failed attempt was refused its own failed step: %v", err)
	}
}
