// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// exitRangeReceipt is a failed attempt whose last step carries the exit code
// the attempt states, which is the shape the executor produces. Each test
// changes exactly the number it is about.
func exitRangeReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	exit := 2
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
				DurationNS: int64(5 * time.Second), ExitCode: 2},
		},
	}
}

// statingExit puts the same number on the attempt and on its last step, which
// is the only pairing checkChildExitIsTheLastStepsExit admits, so a refusal
// below is about the number itself rather than about the two halves disagreeing.
func statingExit(t *testing.T, code int) TerminalReceipt {
	t.Helper()
	receipt := exitRangeReceipt(t)
	value := code
	receipt.ChildExitCode = &value
	receipt.Steps[1].ExitCode = code
	return receipt
}

func TestAnOrdinaryChildExitIsAccepted(t *testing.T) {
	if err := exitRangeReceipt(t).Validate(); err != nil {
		t.Fatalf("a receipt stating an ordinary exit code was refused: %v", err)
	}
}

// The whole range a POSIX wait status can carry, and the edge of it.
func TestEveryExitAShellCanReportIsAccepted(t *testing.T) {
	for _, code := range []int{0, 1, 2, 126, 127, 128, 130, 137, 254, 255} {
		receipt := statingExit(t, code)
		if code == 0 {
			// A clean last step under a failed attempt is the honest shape
			// for a failure between steps, which the outcome rules allow.
			receipt.Steps[1].ExitCode = 0
		}
		if err := receipt.Validate(); err != nil {
			t.Fatalf("exit code %d was refused: %v", code, err)
		}
	}
}

// Windows states the whole DWORD, so the codes a reader actually meets on that
// runner are far above 255 and are not this rule's business to refuse.
func TestAWindowsProcessExitIsAccepted(t *testing.T) {
	for _, code := range []int{0xC0000005, 0xC000013A, 0xFFFFFFFF} {
		if err := statingExit(t, code).Validate(); err != nil {
			t.Fatalf("a Windows process exit %#x was refused: %v", code, err)
		}
	}
}

func TestAnAttemptStatingTheNoChildSentinelIsRefused(t *testing.T) {
	receipt := exitRangeReceipt(t)
	sentinel := -1
	receipt.ChildExitCode = &sentinel
	receipt.Steps[1].ExitCode = -1
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("an attempt claiming the no-child sentinel as its child exit was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "child exit -1") || !strings.Contains(err.Error(), "no child decided") {
		t.Fatalf("the refusal did not name the number or say what it means: %v", err)
	}
}

// The sentinel is refused on the attempt's own field rather than downstream as
// a mismatch with the step, so the reader is told what is actually wrong.
func TestTheSentinelIsRefusedBeforeTheMismatchRule(t *testing.T) {
	receipt := exitRangeReceipt(t)
	sentinel := -1
	receipt.ChildExitCode = &sentinel
	receipt.Steps[1].ExitCode = 7
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("the sentinel beside a disagreeing step was accepted: %v", err)
	}
	if strings.Contains(err.Error(), "last step") {
		t.Fatalf("the sentinel was reported as a mismatch rather than as an impossible code: %v", err)
	}
}

func TestEveryNegativeAttemptExitIsRefused(t *testing.T) {
	for _, code := range []int{-1, -2, -255, -1 << 20} {
		receipt := exitRangeReceipt(t)
		value := code
		receipt.ChildExitCode = &value
		receipt.Steps[1].ExitCode = code
		if !errors.Is(receipt.Validate(), ErrInvalid) {
			t.Fatalf("attempt exit %d was accepted", code)
		}
	}
}

// An attempt no child decided says so by stating nothing, which is what the
// agent writes and what this rule leaves alone.
func TestAnAbsentChildExitIsAccepted(t *testing.T) {
	receipt := exitRangeReceipt(t)
	receipt.ChildExitCode = nil
	receipt.Steps[1].ExitCode = -1
	receipt.Outcome = "timed_out"
	receipt.TimedOut = true
	receipt.Steps[1].TimedOut = true
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an attempt reporting no child exit at all was refused: %v", err)
	}
}

// A step carrying the sentinel is a step whose child never exited, which is an
// ordinary timeout and not this rule's business.
func TestAStepStatingTheSentinelIsAccepted(t *testing.T) {
	receipt := exitRangeReceipt(t)
	receipt.ChildExitCode = nil
	receipt.Outcome = "cancelled"
	receipt.Cancelled = true
	receipt.Steps[1].ExitCode = -1
	receipt.Steps[1].Cancelled = true
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a step reporting a child that never exited was refused: %v", err)
	}
}

func TestAStepBelowTheSentinelIsRefused(t *testing.T) {
	for _, code := range []int{-2, -9, -1 << 30} {
		receipt := exitRangeReceipt(t)
		receipt.ChildExitCode = nil
		receipt.Steps[1].ExitCode = code
		err := receipt.Validate()
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("step exit %d was accepted", code)
		}
		if !strings.Contains(err.Error(), "two") || !strings.Contains(err.Error(), "never exited") {
			t.Fatalf("the refusal did not name the step or the sentinel it fell below: %v", err)
		}
	}
}

func TestAnExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	for _, code := range []int{1 << 32, 1<<32 + 1, 1 << 40} {
		receipt := statingExit(t, code)
		err := receipt.Validate()
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("attempt exit %d was accepted", code)
		}
		if !strings.Contains(err.Error(), "wider than a runner") {
			t.Fatalf("the refusal did not say why the number is impossible: %v", err)
		}
	}
}

// The step half of the same bound, reported against the step rather than the
// attempt.
func TestAStepExitWiderThanARunnerCanReadIsRefused(t *testing.T) {
	receipt := exitRangeReceipt(t)
	receipt.ChildExitCode = nil
	receipt.Steps[1].ExitCode = 1 << 33
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step exit wider than a runner can read was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step two") {
		t.Fatalf("the refusal did not name the step: %v", err)
	}
}

// The bound is stated as int64 so a 32-bit build compiles and judges the same
// numbers a 64-bit one does.
func TestTheWidestReportedExitIsTheWholeDWORD(t *testing.T) {
	if widestReportedExit != 4294967295 {
		t.Fatalf("the widest reported exit is %d, not the whole DWORD", widestReportedExit)
	}
	if noChildExitReported != -1 {
		t.Fatalf("the no-child sentinel is %d, not the executor's -1", noChildExitReported)
	}
}

// A step reporting a bad code under an otherwise clean receipt is still
// refused, so the rule does not depend on the attempt having failed.
func TestABadStepExitUnderASucceededAttemptIsRefused(t *testing.T) {
	receipt := exitRangeReceipt(t)
	zero := 0
	receipt.Outcome = "succeeded"
	receipt.ChildExitCode = &zero
	receipt.EvidenceComplete = true
	receipt.FinalLogSequence = 0
	receipt.Steps[1].ExitCode = -4
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatalf("a step exit below the sentinel was accepted under a succeeded attempt")
	}
}
