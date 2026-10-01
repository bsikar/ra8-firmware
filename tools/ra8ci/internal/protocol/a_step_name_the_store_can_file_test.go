// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"
)

// filable is the predicate under test, asked the way every caller asks it:
// through the shared step-name rule rather than on its own.
func filable(name string) bool { return validStepName(name) }

// logChunkNaming is a valid log chunk for one step, so a test changes the step
// name alone and every other field stays a chunk the validator accepts.
func logChunkNaming(step string) LogChunk {
	data := []byte("one line of output\n")
	sum := sha256.Sum256(data)
	return LogChunk{
		SchemaVersion:     Version,
		AssignmentID:      "018f3a1b-7c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID:         "018f3a1b-7c2d-7e4f-9a1b-2c3d4e5f6a7c",
		AssignmentVersion: 1,
		FencingToken:      1,
		Sequence:          1,
		Stream:            "stdout",
		StepName:          step,
		DataBase64:        base64.StdEncoding.EncodeToString(data),
		SHA256:            hex.EncodeToString(sum[:]),
	}
}

func TestAnOrdinaryStepNameIsFilable(t *testing.T) {
	for _, name := range []string{"build", "run-tests", "flash board 3", "build (release)", "ビルド"} {
		if !filable(name) {
			t.Fatalf("step name %q should be filable", name)
		}
	}
}

func TestAStepNameCarryingANULIsRefused(t *testing.T) {
	// A text column holds no NUL whatever its length, so this name fails at
	// the write rather than at the boundary unless the boundary says it.
	if filable("build\x00") {
		t.Fatal("a step name carrying a NUL should be refused")
	}
}

func TestAStepNameCarryingANewlineIsRefused(t *testing.T) {
	// Interior, so TrimSpace leaves it: this is the name that reads as two
	// steps in anything line-oriented.
	if filable("build\nflash") {
		t.Fatal("a step name carrying an interior newline should be refused")
	}
}

func TestAStepNameCarryingATabIsRefused(t *testing.T) {
	if filable("build\tflash") {
		t.Fatal("a step name carrying an interior tab should be refused")
	}
}

func TestAStepNameCarryingAnEscapeSequenceIsRefused(t *testing.T) {
	if filable("build\x1b[31mred") {
		t.Fatal("a step name carrying an escape sequence should be refused")
	}
}

func TestAStepNameCarryingDeleteOrC1IsRefused(t *testing.T) {
	for _, name := range []string{"build\x7f", "build\u0085more", "build\u009bmore"} {
		if filable(name) {
			t.Fatalf("step name %q should be refused", name)
		}
	}
}

func TestAStepNameThatIsNotUTF8IsRefused(t *testing.T) {
	// Ranging over invalid bytes yields U+FFFD, which is printable, so the
	// encoding has to be judged before the runes are.
	if filable(string([]byte{'b', 'u', 0xff, 'd'})) {
		t.Fatal("a step name that is not valid UTF-8 should be refused")
	}
}

func TestTheOlderStepNameRulesStillHold(t *testing.T) {
	if filable("") || filable(" build") || filable("build ") || filable(strings.Repeat("b", 129)) {
		t.Fatal("empty, untrimmed and oversized step names should still be refused")
	}
	if !filable(strings.Repeat("b", 128)) {
		t.Fatal("a step name at the 128-byte bound should still be accepted")
	}
}

func TestLogChunkValidateRefusesAnUnfilableStepName(t *testing.T) {
	chunk := logChunkNaming("build")
	if err := chunk.Validate(); err != nil {
		t.Fatalf("the ordinary chunk should validate: %v", err)
	}
	// The log chunk stated its own copy of the step-name rule and so never
	// learned this one; it asks the shared rule now.
	chunk.StepName = "build\x00"
	if err := chunk.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a chunk naming an unfilable step should be refused, got %v", err)
	}
}

func TestArtifactMessagesRefuseAnUnfilableStepName(t *testing.T) {
	data := []byte("artifact bytes")
	sum := sha256.Sum256(data)
	chunk := ArtifactChunk{
		SchemaVersion:     Version,
		AssignmentID:      "018f3a1b-7c2d-7e4f-8a1b-2c3d4e5f6a7b",
		AttemptID:         "018f3a1b-7c2d-7e4f-9a1b-2c3d4e5f6a7c",
		AssignmentVersion: 1,
		FencingToken:      1,
		StepName:          "build\nflash",
		Path:              "out/report.txt",
		Sequence:          1,
		Offset:            0,
		DataBase64:        base64.StdEncoding.EncodeToString(data),
		SHA256:            hex.EncodeToString(sum[:]),
	}
	if err := chunk.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an artifact chunk naming an unfilable step should be refused, got %v", err)
	}
	manifest := ArtifactManifest{
		SchemaVersion:     Version,
		AssignmentID:      chunk.AssignmentID,
		AttemptID:         chunk.AttemptID,
		AssignmentVersion: 1,
		FencingToken:      1,
		StepName:          chunk.StepName,
		Path:              chunk.Path,
		TotalBytes:        int64(len(data)),
		SHA256:            hex.EncodeToString(sum[:]),
		FinalSequence:     1,
		CapturedAt:        time.Now(),
	}
	if err := manifest.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a manifest naming an unfilable step should be refused, got %v", err)
	}
}

func TestReceiptRefusesAStepNamedInAShapeEvidenceCannotCarry(t *testing.T) {
	receipt := twoStepReceipt(t)
	if err := receipt.Validate(); err != nil {
		t.Fatalf("the ordinary receipt should validate: %v", err)
	}
	receipt.Steps[0].Name = "build\x1b[2Jclear"
	if err := receipt.Validate(); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a receipt naming an unfilable step should be refused, got %v", err)
	}
}
