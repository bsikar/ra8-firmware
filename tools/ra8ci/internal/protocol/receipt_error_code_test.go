// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
)

// reportingReceipt is an incomplete failed receipt, the only shape that can
// carry an error code at all, so each test below changes exactly the pair it
// is about.
func reportingReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	receipt := evidenceReceipt(t)
	receipt.Outcome, receipt.ChildExitCode, receipt.EvidenceComplete = "failed", nil, false
	receipt.ErrorCode = "log_upload_error"
	return receipt
}

func TestAnIncompleteReceiptNamingItsErrorIsAccepted(t *testing.T) {
	if err := reportingReceipt(t).Validate(); err != nil {
		t.Fatalf("an incomplete receipt naming why was refused: %v", err)
	}
}

func TestEveryCodeTheAgentWritesIsAccepted(t *testing.T) {
	// The vocabulary is transcribed from agent.go's terminalReceipt rather
	// than read from the map under test, so a code quietly dropped from the
	// map fails here instead of passing by agreeing with itself.
	for _, code := range []string{"executor_error", "log_upload_error", "artifact_upload_error"} {
		receipt := reportingReceipt(t)
		receipt.ErrorCode = code
		if err := receipt.Validate(); err != nil {
			t.Fatalf("the agent's own %s receipt was refused: %v", code, err)
		}
	}
	empty := reportingReceipt(t)
	empty.ErrorCode, empty.Steps, empty.FinalLogSequence = "no_step_executed", nil, 0
	if err := empty.Validate(); err != nil {
		t.Fatalf("a receipt reporting that nothing ran was refused: %v", err)
	}
}

func TestAnErrorCodeNoAgentReportsIsRefused(t *testing.T) {
	for _, code := range []string{"boom", "LOG_UPLOAD_ERROR", "log_upload_error ", "run_cancelled_before_ack", "executor_error;drop"} {
		receipt := reportingReceipt(t)
		receipt.ErrorCode = code
		err := receipt.Validate()
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("error code %q was admitted: %v", code, err)
		}
		if !strings.Contains(err.Error(), "no agent reports") {
			t.Fatalf("the refusal of %q does not say the code is not one of ours: %v", code, err)
		}
	}
}

func TestAPlaneAuthoredReasonIsNotAnAgentCode(t *testing.T) {
	// The two reasons the plane writes itself land in the same column as an
	// agent's code (store/dispatch.go and dispatch_reaper.go), so an agent
	// stating one of them would be indistinguishable from the plane's own
	// record of an attempt no agent finished.
	for _, reason := range []string{"run_cancelled_before_ack", "assignment_deadline_without_terminal"} {
		receipt := reportingReceipt(t)
		receipt.ErrorCode = reason
		if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
			t.Fatalf("an agent claiming the plane's reason %q was admitted: %v", reason, err)
		}
	}
}

func TestCompleteEvidenceBesideAnErrorIsRefused(t *testing.T) {
	receipt := reportingReceipt(t)
	receipt.EvidenceComplete = true
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt claiming complete evidence and an error was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "complete evidence and the error log_upload_error") {
		t.Fatalf("the refusal does not name both halves: %v", err)
	}
}

func TestASucceededReceiptCannotCarryAnError(t *testing.T) {
	// Succeeded already requires complete evidence in the outcome switch, so
	// this pins the pair a reader actually cares about rather than a second
	// rule: there is no arrangement of a succeeded receipt that names a code.
	receipt := evidenceReceipt(t)
	receipt.ErrorCode = "executor_error"
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a succeeded receipt naming an executor error was admitted: %v", err)
	}
}

func TestNoStepExecutedBesideStepsIsRefused(t *testing.T) {
	receipt := reportingReceipt(t)
	receipt.ErrorCode = "no_step_executed"
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt reporting no step executed while listing one was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "no step executed and reports 1") {
		t.Fatalf("the refusal does not name the steps the receipt carries: %v", err)
	}
}

func TestOtherCodesAreLeftAloneBesideSteps(t *testing.T) {
	// Only no_step_executed says anything about the step list. The other
	// three name a half of the attempt that failed after steps ran, so the
	// steps beside them are the evidence the code is reporting on.
	for _, code := range []string{"executor_error", "log_upload_error", "artifact_upload_error"} {
		receipt := reportingReceipt(t)
		receipt.ErrorCode = code
		if err := receipt.Validate(); err != nil {
			t.Fatalf("%s beside the steps it reports on was refused: %v", code, err)
		}
	}
}

func TestAnIncompleteReceiptNamingNoCodeIsLeftAlone(t *testing.T) {
	receipt := reportingReceipt(t)
	receipt.ErrorCode = ""
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an incomplete receipt stating no code was refused: %v", err)
	}
}

func TestTheRuleJudgesOnlyTheCode(t *testing.T) {
	// A receipt that passes every other rule and states a good code is
	// accepted, and the same receipt with the code alone changed is refused,
	// so the refusal above is this rule's and not another's.
	receipt := reportingReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the base receipt is not valid, so the tests above prove nothing: %v", err)
	}
	receipt.ErrorCode = "unheard_of"
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("changing only the code did not change the verdict: %v", err)
	}
}
