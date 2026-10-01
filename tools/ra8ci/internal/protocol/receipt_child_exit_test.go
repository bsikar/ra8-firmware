// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// childExitReceipt is a failed attempt whose one step carries the exit code the
// attempt states, which is the shape the executor produces: the attempt's code
// is a copy of the last step's. Each test below changes exactly the half it is
// about.
func childExitReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	exit := 3
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
				DurationNS: int64(5 * time.Second), ExitCode: 3},
		},
	}
}

func TestTheExecutorsOwnChildExitIsAccepted(t *testing.T) {
	if err := childExitReceipt(t).Validate(); err != nil {
		t.Fatalf("an attempt stating the exit code of its own last step was refused: %v", err)
	}
}

func TestAChildExitNoStepReportsIsRefused(t *testing.T) {
	receipt := childExitReceipt(t)
	receipt.Steps[1].ExitCode = 0
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("an attempt claiming a failing child over a clean last step was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "child exit 3") || !strings.Contains(err.Error(), "two") ||
		!strings.Contains(err.Error(), "exited 0") {
		t.Fatalf("the refusal did not name both numbers and the step: %v", err)
	}
}

// The other direction of the same mismatch: the attempt claims a clean child
// while the step it ran last says otherwise.
func TestACleanChildExitOverAFailingLastStepIsRefused(t *testing.T) {
	receipt := childExitReceipt(t)
	zero := 0
	receipt.ChildExitCode = &zero
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an attempt claiming a clean child over a failing last step was accepted: %v", err)
	}
}

func TestEveryDisagreeingPairIsRefused(t *testing.T) {
	for _, test := range []struct {
		name     string
		attempt  int
		lastStep int
	}{
		{"clean attempt over a failing step", 0, 1},
		{"failing attempt over a clean step", 1, 0},
		{"two different failures", 2, 3},
		{"a byte apart", 126, 127},
		{"the signal codes shells report", 130, 137},
		{"the widest pair", 0, 255},
	} {
		t.Run(test.name, func(t *testing.T) {
			receipt := childExitReceipt(t)
			attempt := test.attempt
			receipt.ChildExitCode = &attempt
			receipt.Steps[1].ExitCode = test.lastStep
			if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
				t.Fatalf("attempt %d beside last step %d was accepted: %v", test.attempt, test.lastStep, err)
			}
		})
	}
}

func TestEveryAgreeingPairIsAccepted(t *testing.T) {
	for _, code := range []int{0, 1, 2, 3, 42, 126, 127, 130, 137, 255} {
		receipt := childExitReceipt(t)
		value := code
		receipt.ChildExitCode = &value
		receipt.Steps[1].ExitCode = code
		if code == 0 {
			receipt.Steps = receipt.Steps[:1]
		}
		if err := receipt.Validate(); err != nil {
			t.Fatalf("attempt and last step both exiting %d were refused: %v", code, err)
		}
	}
}

// An attempt that ended without a child exit states no code, which is the
// ordinary shape for a timeout, a cancellation, or a failure between steps.
func TestAnAbsentChildExitIsLeftAlone(t *testing.T) {
	receipt := childExitReceipt(t)
	receipt.ChildExitCode = nil
	receipt.Steps[1].ExitCode = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("an attempt stating no child exit was refused: %v", err)
	}
	receipt.Steps[1].ExitCode = 9
	receipt.Steps[1].TimedOut, receipt.TimedOut, receipt.Outcome = true, true, "timed_out"
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a timed out attempt carrying its own step's code was refused: %v", err)
	}
}

// A receipt carrying no steps has no step to attribute a code to, and the
// error-code rule already holds the no_step_executed case on its own terms.
func TestAReceiptWithoutStepsIsLeftAlone(t *testing.T) {
	receipt := childExitReceipt(t)
	receipt.Steps = nil
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt carrying no steps was refused: %v", err)
	}
}

// The rule reads the LAST step, the only one the executor could have copied
// from, and says nothing about the ones before it.
func TestOnlyTheLastStepIsRead(t *testing.T) {
	receipt := childExitReceipt(t)
	receipt.Steps = append(receipt.Steps, StepSummary{Name: "three",
		StartedAt: receipt.StartedAt.Add(9 * time.Second), EndedAt: receipt.EndedAt,
		DurationNS: int64(time.Second), ExitCode: 3})
	receipt.Steps[1].ExitCode = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a middle step exiting clean under a failing last step was refused: %v", err)
	}
}

// A mid-list failure is the failure-order rule's refusal, not this one's: that
// receipt is wrong about which step ended the attempt before it is wrong about
// the code, and the message a reader gets has to say so.
func TestAMidListFailureIsReportedAsOne(t *testing.T) {
	receipt := childExitReceipt(t)
	receipt.Steps[0].ExitCode, receipt.Steps[1].ExitCode = 3, 0
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a mid-list failure was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "steps reported after it") {
		t.Fatalf("the refusal was not the failure-order one: %v", err)
	}
}

// A succeeded attempt is held to 0 by the outcome switch and to clean steps by
// checkStepOutcomesAgreeWithTheAttempt, so the pair this rule reads is already
// pinned there; it must not start refusing the shape those rules accept.
func TestASucceededReceiptStillPasses(t *testing.T) {
	if err := evidenceReceipt(t).Validate(); err != nil {
		t.Fatalf("a clean succeeded receipt was refused: %v", err)
	}
}
