// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// stepless builds the shape this rule is about: a receipt whose attempt-level
// fields are all coherent and whose step list is empty. Every field is set the
// way an agent would set it for the outcome asked for, so a refusal can only
// come from the step list being empty.
func stepless(outcome string, evidenceComplete bool) TerminalReceipt {
	started := time.Date(2026, 9, 27, 10, 0, 0, 0, time.UTC)
	facts := HostFacts{Cores: 4, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: started}
	receipt := TerminalReceipt{
		SchemaVersion:        Version,
		AssignmentID:         "018f4c2a-7b1c-7def-8a90-1234567890ab",
		AttemptID:            "018f4c2a-7b1c-7def-8a91-1234567890ab",
		AssignmentVersion:    1,
		FencingToken:         1,
		Outcome:              outcome,
		EvidenceComplete:     evidenceComplete,
		StartedAt:            started,
		EndedAt:              started.Add(time.Second),
		DurationNS:           int64(time.Second),
		CatalogSHA256:        strings.Repeat("a", 64),
		SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart:     facts,
		HostFactsAtEnd: HostFacts{Cores: 4, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
			LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: started.Add(time.Second)},
	}
	switch outcome {
	case "succeeded":
		zero := 0
		receipt.ChildExitCode = &zero
	case "timed_out":
		receipt.TimedOut = true
	case "cancelled":
		receipt.Cancelled = true
	}
	if !evidenceComplete {
		receipt.ErrorCode = "no_step_executed"
	}
	return receipt
}

// withStep gives a receipt the one step that makes it a report of work done.
func withStep(receipt TerminalReceipt) TerminalReceipt {
	receipt.Steps = []StepSummary{{
		Name:       "build",
		StartedAt:  receipt.StartedAt,
		EndedAt:    receipt.EndedAt,
		DurationNS: int64(time.Second),
	}}
	return receipt
}

func TestSuccessWithNoStepsIsRefused(t *testing.T) {
	err := checkSuccessNamesTheStepsItRan(stepless("succeeded", true))
	if err == nil {
		t.Fatal("a succeeded attempt reporting no step was admitted")
	}
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("refusal does not wrap ErrInvalid: %v", err)
	}
	if !strings.Contains(err.Error(), "succeeded") {
		t.Fatalf("refusal does not name the claim it refuses: %v", err)
	}
}

func TestCompleteEvidenceWithNoStepsIsRefused(t *testing.T) {
	receipt := stepless("failed", true)
	receipt.ErrorCode = ""
	err := checkSuccessNamesTheStepsItRan(receipt)
	if err == nil {
		t.Fatal("complete evidence reporting no step was admitted")
	}
	if !strings.Contains(err.Error(), "complete evidence") {
		t.Fatalf("refusal does not name the claim it refuses: %v", err)
	}
}

func TestSuccessIsRefusedBeforeTheEvidenceFlagIsRead(t *testing.T) {
	receipt := stepless("succeeded", true)
	err := checkSuccessNamesTheStepsItRan(receipt)
	if err == nil || !strings.Contains(err.Error(), "succeeded") {
		t.Fatalf("a stepless success must be refused as a success: %v", err)
	}
}

func TestFailedAttemptWithNoStepsIsAdmitted(t *testing.T) {
	if err := checkSuccessNamesTheStepsItRan(stepless("failed", false)); err != nil {
		t.Fatalf("an attempt that failed before its first step was refused: %v", err)
	}
}

func TestTimedOutAttemptWithNoStepsIsAdmitted(t *testing.T) {
	if err := checkSuccessNamesTheStepsItRan(stepless("timed_out", false)); err != nil {
		t.Fatalf("an attempt that timed out before its first step was refused: %v", err)
	}
}

func TestCancelledAttemptWithNoStepsIsAdmitted(t *testing.T) {
	if err := checkSuccessNamesTheStepsItRan(stepless("cancelled", false)); err != nil {
		t.Fatalf("an attempt cancelled before its first step was refused: %v", err)
	}
}

func TestSuccessWithOneStepIsAdmitted(t *testing.T) {
	if err := checkSuccessNamesTheStepsItRan(withStep(stepless("succeeded", true))); err != nil {
		t.Fatalf("a succeeded attempt naming its step was refused: %v", err)
	}
}

func TestCompleteEvidenceWithOneStepIsAdmitted(t *testing.T) {
	receipt := withStep(stepless("failed", true))
	receipt.ErrorCode = ""
	if err := checkSuccessNamesTheStepsItRan(receipt); err != nil {
		t.Fatalf("a complete receipt naming its step was refused: %v", err)
	}
}

// End to end through Validate: the shape this rule exists for is a receipt that
// satisfies every other rule in the tree, so it must be Validate that refuses
// it, not just the predicate on its own.
func TestValidateRefusesASteplessSuccess(t *testing.T) {
	receipt := stepless("succeeded", true)
	if err := receipt.Validate(); err == nil {
		t.Fatal("Validate admitted a succeeded attempt that ran nothing")
	}
	if err := withStep(receipt).Validate(); err != nil {
		t.Fatalf("Validate refused the same receipt once it named its step: %v", err)
	}
}

func TestValidateRefusesSteplessCompleteEvidence(t *testing.T) {
	receipt := stepless("failed", true)
	receipt.ErrorCode = ""
	if err := receipt.Validate(); err == nil {
		t.Fatal("Validate admitted complete evidence for an attempt that ran nothing")
	}
}

// The honest shape must still pass the whole tree, not only the predicate: an
// attempt that ended before its first step reports no_step_executed, states
// incomplete evidence, and is the receipt this rule must leave alone.
func TestValidateAdmitsAnAttemptThatEndedBeforeItsFirstStep(t *testing.T) {
	if err := stepless("failed", false).Validate(); err != nil {
		t.Fatalf("Validate refused an honest no_step_executed receipt: %v", err)
	}
}
