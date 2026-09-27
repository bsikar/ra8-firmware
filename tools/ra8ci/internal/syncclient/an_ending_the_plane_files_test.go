// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func ending(steps ...executor.StepResult) spool.Entry {
	finished := time.Unix(200, 0).UTC()
	return spool.Entry{
		ID:         "0123456789abcdef0123456789abcdef",
		StartedAt:  time.Unix(100, 0).UTC(),
		FinishedAt: &finished,
		Result:     &executor.Result{Steps: steps},
	}
}

func TestAStepThatSimplyRanIsUploaded(t *testing.T) {
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(
		ending(executor.StepResult{Name: "build"}, executor.StepResult{Name: "test"})); err != nil {
		t.Fatalf("ordinary steps refused: %v", err)
	}
}

func TestAStepThatRanOutOfTimeIsUploaded(t *testing.T) {
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(
		ending(executor.StepResult{Name: "test", TimedOut: true})); err != nil {
		t.Fatalf("a timed-out step refused: %v", err)
	}
}

func TestAStepThatWasCalledOffIsUploaded(t *testing.T) {
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(
		ending(executor.StepResult{Name: "test", Cancelled: true})); err != nil {
		t.Fatalf("a cancelled step refused: %v", err)
	}
}

func TestAStepEndingTwoWaysAtOnceStopsTheSweep(t *testing.T) {
	err := checkUploadedStepEndingsAreOnesThePlaneFiles(ending(
		executor.StepResult{Name: "build"},
		executor.StepResult{Name: "test", TimedOut: true, Cancelled: true},
	))
	if !errors.Is(err, ErrContradictoryEnding) {
		t.Fatalf("a step that ended two ways accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step 1") {
		t.Fatalf("the refusal did not name the step: %v", err)
	}
}

func TestARecordWithNoResultStatesNoEnding(t *testing.T) {
	entry := ending()
	entry.Result = nil
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(entry); err != nil {
		t.Fatalf("a record without a result was judged for step endings: %v", err)
	}
}

func TestARecordCarryingNoStepsStatesNoEnding(t *testing.T) {
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(ending()); err != nil {
		t.Fatalf("a stepless record refused: %v", err)
	}
}

func TestTheAttemptLevelPairIsLeftToTheDoorThatChooses(t *testing.T) {
	// offlineInput does not refuse an attempt that states both, it reads
	// TimedOut first and files the run as timed_out. Refusing it here would
	// throw away evidence the plane accepts.
	entry := ending(executor.StepResult{Name: "build"})
	entry.Result.TimedOut = true
	entry.Result.Cancelled = true
	if err := checkUploadedStepEndingsAreOnesThePlaneFiles(entry); err != nil {
		t.Fatalf("the door judged the attempt-level pair: %v", err)
	}
}
