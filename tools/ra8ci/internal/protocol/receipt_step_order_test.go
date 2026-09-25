// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// timelineReceipt is a receipt that passes every other rule Validate states and
// carries two sequential steps, so each test below changes exactly the stamp it
// is about.
func timelineReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	zero := 0
	now := time.Now().UTC()
	facts := HostFacts{Cores: 2, RAMBytes: 1 << 30, RAMFreeBytes: 1 << 29, Load1: 0.5,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: now}
	return TerminalReceipt{
		SchemaVersion: Version, AssignmentID: "018f8b3a-1c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID: "018f8b3a-1c2d-7e4f-9a1b-2c3d4e5f6a7c", AssignmentVersion: 1, FencingToken: 1,
		Outcome: "succeeded", ChildExitCode: &zero, EvidenceComplete: true,
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

func TestSequentialStepsAreAccepted(t *testing.T) {
	receipt := timelineReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the executor's own sequential steps were refused: %v", err)
	}
}

func TestStepStartingWhenThePreviousStepEndedIsAccepted(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[1].StartedAt = receipt.Steps[0].EndedAt
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a step starting exactly at the previous end was refused: %v", err)
	}
}

func TestStepStartingBeforeThePreviousStepEndedIsRefused(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[1].StartedAt = receipt.Steps[0].EndedAt.Add(-time.Second)
	receipt.Steps[1].DurationNS = int64(receipt.Steps[1].EndedAt.Sub(receipt.Steps[1].StartedAt))
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("overlapping steps were accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step two starts before step one ended") {
		t.Fatalf("the refusal does not name both steps: %v", err)
	}
}

func TestStepsReportedOutOfOrderAreRefused(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[0], receipt.Steps[1] = receipt.Steps[1], receipt.Steps[0]
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("steps listed in reverse order were accepted: %v", err)
	}
}

func TestStepStartingBeforeItsAttemptIsRefused(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[0].StartedAt = receipt.StartedAt.Add(-time.Hour)
	receipt.Steps[0].DurationNS = int64(receipt.Steps[0].EndedAt.Sub(receipt.Steps[0].StartedAt))
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step starting before its own attempt was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step one starts before the attempt") {
		t.Fatalf("the refusal does not name the step: %v", err)
	}
}

func TestStepEndingAfterItsAttemptIsRefused(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[1].EndedAt = receipt.EndedAt.Add(time.Hour)
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step ending after its own attempt was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step two ends after the attempt") {
		t.Fatalf("the refusal does not name the step: %v", err)
	}
}

func TestStepStampsWithinTheClockAllowanceAreAccepted(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps[0].StartedAt = receipt.StartedAt.Add(-time.Second)
	receipt.Steps[0].DurationNS = int64(receipt.Steps[0].EndedAt.Sub(receipt.Steps[0].StartedAt))
	receipt.Steps[1].EndedAt = receipt.EndedAt.Add(time.Second)
	receipt.Steps[1].DurationNS = int64(receipt.Steps[1].EndedAt.Sub(receipt.Steps[1].StartedAt))
	receipt.DurationNS = int64(receipt.Steps[1].EndedAt.Sub(receipt.Steps[0].StartedAt))
	if err := receipt.Validate(); err != nil {
		t.Fatalf("stamps a second apart from their attempt were refused: %v", err)
	}
}

func TestSingleStepAndEmptyStepsKeepPassing(t *testing.T) {
	receipt := timelineReceipt(t)
	receipt.Steps = receipt.Steps[:1]
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a single step was refused: %v", err)
	}
	receipt.Steps = nil
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	receipt.ChildExitCode = nil
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt with no steps was refused: %v", err)
	}
}
