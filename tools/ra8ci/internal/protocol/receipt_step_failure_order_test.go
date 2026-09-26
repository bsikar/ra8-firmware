// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// failureOrderReceipt carries two sequential steps under a failed attempt, so
// each test below changes exactly the verdict it is about. It is written out
// rather than built from evidenceReceipt because every test here needs an
// attempt that is allowed to report a failure at all.
func failureOrderReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	exit := 1
	now := time.Now().UTC()
	facts := HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: now}
	return TerminalReceipt{
		SchemaVersion: Version, AssignmentID: "018f8b3a-1c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID: "018f8b3a-1c2d-7e4f-9a1b-2c3d4e5f6a7c", AssignmentVersion: 1, FencingToken: 1,
		Outcome: "failed", ChildExitCode: &exit,
		StartedAt: now, EndedAt: now.Add(10 * time.Second), DurationNS: int64(10 * time.Second),
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart: facts, HostFactsAtEnd: facts,
		Steps: []StepSummary{
			{Name: "one", StartedAt: now, EndedAt: now.Add(4 * time.Second),
				DurationNS: int64(4 * time.Second)},
			{Name: "two", StartedAt: now.Add(4 * time.Second), EndedAt: now.Add(9 * time.Second),
				DurationNS: int64(5 * time.Second)},
		},
	}
}

func TestAFailureInTheLastStepIsAccepted(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Steps[1].ExitCode = 1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the executor's own failing last step was refused: %v", err)
	}
}

func TestCleanStepsUnderAFailedAttemptAreAccepted(t *testing.T) {
	if err := failureOrderReceipt(t).Validate(); err != nil {
		t.Fatalf("an attempt that failed around its steps was refused: %v", err)
	}
}

func TestAnEarlierStepExitingNonZeroIsRefused(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Steps[0].ExitCode = 1
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a mid-list failing step was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "one") {
		t.Fatalf("the refusal did not name the step that failed: %v", err)
	}
}

func TestAnEarlierStepTimingOutIsRefused(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Outcome, receipt.TimedOut, receipt.ChildExitCode = "timed_out", true, nil
	receipt.Steps[0].TimedOut = true
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step reported as timed out with work after it was accepted: %v", err)
	}
}

func TestAnEarlierStepCancelledIsRefused(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Outcome, receipt.Cancelled, receipt.ChildExitCode = "cancelled", true, nil
	receipt.Steps[0].Cancelled = true
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step reported as cancelled with work after it was accepted: %v", err)
	}
}

// A single step is the last step, so the rule has nothing to say about it and
// the attempt's own verdict is judged by the outcome switch alone.
func TestASingleFailingStepIsAccepted(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Steps = receipt.Steps[:1]
	receipt.Steps[0].ExitCode = 1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a lone failing step was refused: %v", err)
	}
}

func TestAReceiptWithNoStepsIsLeftAlone(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Steps = nil
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt carrying no steps was refused: %v", err)
	}
}

// The rule runs on the order the steps are reported in, which the timeline
// check has already held to the order they ran in.
func TestTheLastStepIsTheOneReportedLast(t *testing.T) {
	receipt := failureOrderReceipt(t)
	receipt.Steps[0].ExitCode, receipt.Steps[1].ExitCode = 0, 1
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a failure in the last reported step was refused: %v", err)
	}
	receipt.Steps[0].ExitCode, receipt.Steps[1].ExitCode = 1, 0
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("the same two verdicts in the other order were accepted: %v", err)
	}
}
