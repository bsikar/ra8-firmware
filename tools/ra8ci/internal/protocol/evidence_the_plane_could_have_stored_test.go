// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// budgetReceipt is sequenceReceipt with its one step's stdout set to bytes and
// a sequence that covers them, so the rule under test is the only one a change
// here can trip.
func budgetReceipt(t *testing.T, bytes int64) TerminalReceipt {
	t.Helper()
	receipt := sequenceReceipt(t)
	receipt.Steps[0].StdoutBytes = bytes
	receipt.FinalLogSequence = chunksFor(bytes)
	return receipt
}

func TestAnOrdinaryUploadFitsTheBudget(t *testing.T) {
	if err := budgetReceipt(t, 26).Validate(); err != nil {
		t.Fatalf("a receipt reporting one line of output was refused: %v", err)
	}
}

func TestExactlyTheByteBudgetIsAccepted(t *testing.T) {
	if err := budgetReceipt(t, maxAttemptLogBytes).Validate(); err != nil {
		t.Fatalf("a receipt filling the budget exactly was refused: %v", err)
	}
}

func TestOneByteOverTheBudgetIsRefused(t *testing.T) {
	err := budgetReceipt(t, maxAttemptLogBytes+1).Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt claiming more bytes than the plane accepts was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "more log bytes") {
		t.Fatalf("the refusal does not say which budget was passed: %v", err)
	}
}

// The budget belongs to the attempt, not to a step, so two steps that each fit
// still have to fit together.
func TestTwoStepsShareOneBudget(t *testing.T) {
	receipt := budgetReceipt(t, maxAttemptLogBytes/2+1)
	first := receipt.Steps[0]
	receipt.Steps = append(receipt.Steps, StepSummary{
		Name: "selftest", StartedAt: first.EndedAt, EndedAt: first.EndedAt,
		StdoutSHA256: first.StdoutSHA256, StdoutBytes: maxAttemptLogBytes/2 + 1,
		StderrSHA256: emptyStreamDigest,
	})
	receipt.FinalLogSequence = chunksFor(maxAttemptLogBytes/2+1) * 2
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("two steps between them passed the budget and were admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "more log bytes") {
		t.Fatalf("the refusal does not say which budget was passed: %v", err)
	}
}

// Both streams of one step are counted, the way the uploader charges for both.
func TestBothStreamsOfAStepAreCounted(t *testing.T) {
	receipt := budgetReceipt(t, maxAttemptLogBytes/2+1)
	receipt.Steps[0].StderrSHA256 = receipt.Steps[0].StdoutSHA256
	receipt.Steps[0].StderrBytes = maxAttemptLogBytes/2 + 1
	receipt.FinalLogSequence = chunksFor(maxAttemptLogBytes/2+1) * 2
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a step whose two streams pass the budget was admitted: %v", err)
	}
}

func TestExactlyTheChunkBudgetIsAccepted(t *testing.T) {
	receipt := budgetReceipt(t, 26)
	receipt.FinalLogSequence = maxAttemptLogChunks
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt naming the last chunk the plane accepts was refused: %v", err)
	}
}

func TestOneChunkOverTheBudgetIsRefused(t *testing.T) {
	receipt := budgetReceipt(t, 26)
	receipt.FinalLogSequence = maxAttemptLogChunks + 1
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt naming a chunk past the budget was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "log chunks") {
		t.Fatalf("the refusal does not name the chunk budget: %v", err)
	}
}

// The chunk bound is not implied by the byte bound: a step printing a line at a
// time spends a chunk a line, so few bytes can still name many chunks.
func TestManyChunksCarryingFewBytesAreStillBounded(t *testing.T) {
	receipt := budgetReceipt(t, 4096)
	receipt.FinalLogSequence = 100000
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt naming a hundred thousand chunks of four kilobytes was admitted: %v", err)
	}
}

// An incomplete receipt is reporting the refusal rather than contradicting it:
// the bytes were written, the upload did not land, and agent.go says so.
func TestAnIncompleteReceiptMayReportMoreThanTheBudget(t *testing.T) {
	receipt := budgetReceipt(t, maxAttemptLogBytes*4)
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	receipt.ErrorCode = "log_upload_error"
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a reported log upload failure was refused: %v", err)
	}
}

// A byte count near the width of the type is client JSON like any other, and
// the headroom comparison must refuse it rather than wrap into acceptance.
func TestTheWidestByteCountIsRefusedNotWrapped(t *testing.T) {
	const maxInt64 = int64(^uint64(0) >> 1)
	receipt := budgetReceipt(t, 26)
	receipt.Steps[0].StdoutBytes = maxInt64
	receipt.FinalLogSequence = maxAttemptLogChunks
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("the widest byte count was admitted: %v", err)
	}
}

// A stepless receipt keeps the rules it already had: there are no bytes to
// charge, and the sequence it states is still held to the chunk budget.
func TestASteplessReceiptIsLeftToItsOwnRules(t *testing.T) {
	receipt := budgetReceipt(t, 26)
	receipt.Steps = nil
	receipt.FinalLogSequence = 0
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	receipt.ErrorCode = "no_step_executed"
	receipt.EndedAt = receipt.StartedAt.Add(time.Second)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a stepless receipt was refused by the budget rule: %v", err)
	}
}
