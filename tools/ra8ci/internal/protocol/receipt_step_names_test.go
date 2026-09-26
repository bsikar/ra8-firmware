// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// twoStepReceipt is evidenceReceipt with a second, silent step after the first,
// so the tests below change exactly the names they are about.
func twoStepReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	receipt := evidenceReceipt(t)
	first := receipt.Steps[0]
	second := StepSummary{
		Name:         "selftest",
		StartedAt:    first.EndedAt,
		EndedAt:      first.EndedAt,
		StdoutSHA256: emptyStreamDigest,
		StderrSHA256: emptyStreamDigest,
	}
	receipt.Steps = append(receipt.Steps, second)
	receipt.EndedAt = second.EndedAt.Add(time.Millisecond)
	receipt.DurationNS = receipt.EndedAt.Sub(receipt.StartedAt).Nanoseconds()
	return receipt
}

func TestDistinctlyNamedStepsAreAccepted(t *testing.T) {
	if err := twoStepReceipt(t).Validate(); err != nil {
		t.Fatalf("a receipt naming its steps distinctly was refused: %v", err)
	}
}

func TestTwoStepsSharingOneNameAreRefused(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps[1].Name = receipt.Steps[0].Name
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt naming two steps alike was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "two steps") {
		t.Fatalf("the refusal does not name what collided: %v", err)
	}
}

// The name a receipt states is the name a chunk must carry to be filed under
// that step, so a name LogChunk.Validate would refuse is unattributable.
func TestAnUntrimmedStepNameIsRefused(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps[1].Name = "selftest "
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step named with trailing whitespace was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "log chunks cannot carry") {
		t.Fatalf("the refusal does not say why the name cannot be used: %v", err)
	}
}

func TestAStepNamePastTheChunkBoundIsRefused(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps[1].Name = strings.Repeat("s", 129)
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step name past the bound every chunk is held to was admitted: %v", err)
	}
}

func TestAStepNameAtTheChunkBoundIsAccepted(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps[1].Name = strings.Repeat("s", 128)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a step name exactly at the bound was refused: %v", err)
	}
}

// Validate refuses an empty name in its own per-step loop, so the rule is
// checked here directly: it states the whole shape rather than half of it.
func TestAnEmptyStepNameIsRefusedByTheRuleItself(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps[1].Name = ""
	if err := checkStepNamesAttributeEvidence(receipt); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an unnamed step was admitted: %v", err)
	}
}

// The names a real chunk and a real manifest carry are judged by the same rule,
// so a name one of them can carry is a name a receipt may state.
func TestTheReceiptAndItsEvidenceJudgeANameAlike(t *testing.T) {
	for _, name := range []string{"selftest", "selftest ", "", strings.Repeat("s", 129)} {
		receipt := twoStepReceipt(t)
		receipt.Steps[1].Name = name
		refusedByReceipt := checkStepNamesAttributeEvidence(receipt) != nil
		if refusedByReceipt != !validStepName(name) {
			t.Fatalf("receipt and evidence disagree about the name %q", name)
		}
	}
}

func TestAReceiptWithNoStepsNamesNothing(t *testing.T) {
	receipt := twoStepReceipt(t)
	receipt.Steps = nil
	receipt.FinalLogSequence = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt carrying no steps was refused: %v", err)
	}
}
