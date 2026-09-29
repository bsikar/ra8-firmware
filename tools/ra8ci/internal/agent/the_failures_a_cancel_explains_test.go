// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// cancelledRun is an attempt whose single step was torn down by a cancel: it
// ran, it wrote its log, and it exited on the signal. Its evidence is whole.
func cancelledRun() executor.Result {
	now := time.Now().UTC()
	return executor.Result{TaskName: "format-check", StartedAt: now, EndedAt: now.Add(time.Second),
		Duration: time.Second, ExitCode: 143, Cancelled: true,
		Steps: []executor.StepResult{{Name: "format-tree-check", StartedAt: now,
			EndedAt: now.Add(time.Second), Duration: time.Second, ExitCode: 143,
			Cancelled: true, StdoutBytes: 10}}}
}

func TestEndedTheAttemptNamesOnlyTheCancellation(t *testing.T) {
	cancelled := cancelledRun()
	timedOut := cancelledRun()
	timedOut.Cancelled, timedOut.TimedOut = false, true

	for _, c := range []struct {
		name   string
		result executor.Result
		err    error
		want   bool
	}{
		{"cancelled, bare", cancelled, context.Canceled, true},
		{"cancelled, wrapped as the executor wraps it", cancelled,
			fmt.Errorf("execute format-tree-check: %w", context.Canceled), true},
		{"cancelled, a join of nothing but the cancellation", cancelled,
			errors.Join(context.Canceled, fmt.Errorf("write stdout: %w", context.Canceled)), true},
		{"cancelled, nothing failed", cancelled, nil, false},
		{"cancelled, an unrelated failure", cancelled, errors.New("broken logs"), false},
		{"cancelled, the deadline rather than a cancel", cancelled, context.DeadlineExceeded, false},
		{"timed out rather than cancelled", timedOut, context.Canceled, false},
		{"neither, and the step merely failed", executor.Result{}, context.Canceled, false},
	} {
		if got := endedTheAttempt(c.result, c.err); got != c.want {
			t.Errorf("%s: endedTheAttempt = %v, want %v", c.name, got, c.want)
		}
	}
}

// A real failure must not be forgiven for arriving next to the cancellation.
func TestEndedTheAttemptRefusesAJoinCarryingARealFailure(t *testing.T) {
	beside := errors.Join(context.Canceled, errors.New("the runner lost the step"))
	if endedTheAttempt(cancelledRun(), beside) {
		t.Fatal("a real failure beside the cancellation was read as the cancellation")
	}
	if endedTheAttempt(cancelledRun(), errors.Join()) {
		t.Fatal("an empty join was read as the cancellation")
	}
}

// The receipt is the point of all this: a cancelled attempt that tore down
// cleanly owes the plane complete evidence and no error code.
func TestCancelledReceiptKeepsItsEvidence(t *testing.T) {
	assignment := testAssignment()
	facts, err := HostFacts()
	if err != nil {
		t.Fatal(err)
	}
	torn := fmt.Errorf("execute format-tree-check: %w", context.Canceled)

	receipt := terminalReceipt(assignment, cancelledRun(), facts, facts, 2, torn, nil, nil)
	if receipt.Outcome != "cancelled" || !receipt.Cancelled || receipt.TimedOut {
		t.Fatalf("receipt does not report the cancellation: %+v", receipt)
	}
	if !receipt.EvidenceComplete || receipt.ErrorCode != "" {
		t.Fatalf("the cancellation degraded the evidence: %+v", receipt)
	}
	if receipt.Validate() != nil {
		t.Fatalf("receipt does not validate: %v", receipt.Validate())
	}

	// The same for the log writer, which the cancel reaches the same way.
	receipt = terminalReceipt(assignment, cancelledRun(), facts, facts, 2, nil, context.Canceled, nil)
	if !receipt.EvidenceComplete || receipt.ErrorCode != "" {
		t.Fatalf("a cancelled log upload degraded the evidence: %+v", receipt)
	}
}

// And a cancelled attempt that ALSO hit a real failure still says so.
func TestCancelledReceiptStillReportsARealFailure(t *testing.T) {
	assignment := testAssignment()
	facts, err := HostFacts()
	if err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		name           string
		runErr, logErr error
		wantCode       string
	}{
		{"a real executor failure beside the cancel",
			errors.Join(context.Canceled, errors.New("the runner lost the step")), nil, "executor_error"},
		{"a real executor failure alone", errors.New("the runner lost the step"), nil, "executor_error"},
		{"a real log failure beside the cancel", nil,
			errors.Join(context.Canceled, errors.New("the plane refused a chunk")), "log_upload_error"},
	} {
		receipt := terminalReceipt(assignment, cancelledRun(), facts, facts, 2, c.runErr, c.logErr, nil)
		if receipt.EvidenceComplete {
			t.Errorf("%s: evidence reported complete: %+v", c.name, receipt)
		}
		if receipt.ErrorCode != c.wantCode {
			t.Errorf("%s: error code = %q, want %q", c.name, receipt.ErrorCode, c.wantCode)
		}
		if receipt.Validate() != nil {
			t.Errorf("%s: receipt does not validate: %v", c.name, receipt.Validate())
		}
	}
}
