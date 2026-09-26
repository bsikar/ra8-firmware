// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
)

// sequenceReceipt is evidenceReceipt with a final log sequence that matches the
// 26 stdout bytes its one step reports: one chunk carried them.
func sequenceReceipt(t *testing.T) TerminalReceipt {
	t.Helper()
	receipt := evidenceReceipt(t)
	receipt.FinalLogSequence = 1
	return receipt
}

func TestOneChunkCarriesOneStepsOutput(t *testing.T) {
	receipt := sequenceReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a receipt whose sequence covers its own bytes was refused: %v", err)
	}
}

func TestCompleteEvidenceWithBytesAndNoChunksIsRefused(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.FinalLogSequence = 0
	err := receipt.Validate()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a complete receipt stating bytes and no chunk was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "fewer than the 1") {
		t.Fatalf("the refusal does not name what the bytes need: %v", err)
	}
}

// A sequence above what the bytes need is left alone, the same way an absent
// digest is: the byte count is the evidence this rule reads.
func TestChunksBesideAnUncapturedByteCountAreLeftAlone(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.Steps[0].StdoutSHA256, receipt.Steps[0].StdoutBytes = emptyStreamDigest, 0
	receipt.FinalLogSequence = 3
	if err := receipt.Validate(); err != nil {
		t.Fatalf("chunks beside an uncaptured byte count were refused: %v", err)
	}
}

func TestAStepThatWroteNothingNeedsNoChunk(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.Steps[0].StdoutSHA256, receipt.Steps[0].StdoutBytes = emptyStreamDigest, 0
	receipt.FinalLogSequence = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a silent step was required to have uploaded a chunk: %v", err)
	}
}

func TestBytesPastOneChunkNeedMoreThanOneChunk(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.Steps[0].StdoutBytes = MaxLogBytes + 1
	receipt.FinalLogSequence = 1
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("more bytes than one chunk can carry were admitted as one chunk: %v", err)
	}
	receipt.FinalLogSequence = 2
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the two chunks those bytes need were refused: %v", err)
	}
}

// Each stream of each step is counted on its own because the uploader never
// mixes two of them into one chunk, so half a chunk of stdout beside half a
// chunk of stderr still needs two.
func TestEachStreamIsCountedOnItsOwn(t *testing.T) {
	receipt := sequenceReceipt(t)
	output := receipt.Steps[0].StdoutSHA256
	receipt.Steps[0].StdoutBytes = MaxLogBytes / 2
	receipt.Steps[0].StderrSHA256, receipt.Steps[0].StderrBytes = output, MaxLogBytes/2
	receipt.FinalLogSequence = 1
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("two streams were admitted as one shared chunk: %v", err)
	}
	receipt.FinalLogSequence = 2
	if err := receipt.Validate(); err != nil {
		t.Fatalf("one chunk per stream was refused: %v", err)
	}
}

// A sequence above what the bytes need is fine: the uploader cuts on write
// boundaries, so a step that printed a line at a time uses a chunk a line.
func TestMoreChunksThanTheMinimumAreAccepted(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.FinalLogSequence = 26
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a chunk per line was refused: %v", err)
	}
}

// An incomplete receipt is reporting the shortfall, not contradicting itself:
// this is exactly the shape agent.go sends with log_upload_error.
func TestAnIncompleteReceiptMayStateFewerChunksThanBytes(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.Outcome, receipt.EvidenceComplete = "failed", false
	receipt.ErrorCode = "log_upload_error"
	receipt.FinalLogSequence = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a reported log upload failure was refused: %v", err)
	}
}

// chunksFor and the running total are held to the width of the type directly:
// a receipt is client JSON and may state byte counts near maxInt64, and a total
// that wrapped negative would turn this refusal into an acceptance.
func TestTheChunkCountNeverWrapsNegative(t *testing.T) {
	const maxInt64 = int64(^uint64(0) >> 1)
	if got := chunksFor(maxInt64); got <= 0 {
		t.Fatalf("the widest byte count needs a positive chunk count, got %d", got)
	}
	if got := chunksFor(-1); got != 0 {
		t.Fatalf("a negative byte count needs no chunk, got %d", got)
	}
	if got := addChunks(maxInt64-1, 5); got != maxInt64 {
		t.Fatalf("the running total wrapped instead of saturating, got %d", got)
	}
}

// The rule reads only the steps and the sequence, so a receipt with no steps at
// all keeps the outcome rules it already had.
func TestAReceiptWithNoStepsKeepsItsOwnRules(t *testing.T) {
	receipt := sequenceReceipt(t)
	receipt.Steps = nil
	receipt.FinalLogSequence = 0
	if err := receipt.Validate(); err != nil {
		t.Fatalf("a stepless receipt was refused by the sequence rule: %v", err)
	}
}
