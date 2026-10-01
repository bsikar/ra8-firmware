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

// windowed builds a record whose envelope runs from second 100 to second 200,
// so a step's stamps can be placed inside it, on its edges, or outside it.
func windowed(steps ...executor.StepResult) spool.Entry {
	finished := time.Unix(200, 0).UTC()
	return spool.Entry{
		ID:         "0123456789abcdef0123456789abcdef",
		StartedAt:  time.Unix(100, 0).UTC(),
		FinishedAt: &finished,
		Result:     &executor.Result{Steps: steps},
	}
}

func ran(name string, from, to int64, duration time.Duration) executor.StepResult {
	return executor.StepResult{
		Name:      name,
		StartedAt: time.Unix(from, 0).UTC(),
		EndedAt:   time.Unix(to, 0).UTC(),
		Duration:  duration,
	}
}

func TestStepsMeasuredInsideTheRecordAreUploaded(t *testing.T) {
	if err := checkUploadedStepWindowsWereMeasured(windowed(
		ran("build", 110, 140, 30*time.Second),
		ran("test", 140, 190, 50*time.Second),
	)); err != nil {
		t.Fatalf("ordinary step windows refused: %v", err)
	}
}

func TestAStepFillingTheWholeRecordIsUploaded(t *testing.T) {
	if err := checkUploadedStepWindowsWereMeasured(windowed(
		ran("build", 100, 200, 100*time.Second),
	)); err != nil {
		t.Fatalf("a step on the record's own edges refused: %v", err)
	}
}

func TestAStepWithNoStampsStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(
		ran("build", 110, 140, 30*time.Second),
		executor.StepResult{Name: "test"},
	))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("a step stating no stamps accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "step 1") {
		t.Fatalf("the refusal did not name the step: %v", err)
	}
}

func TestAStepEndingBeforeItStartedStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(ran("build", 150, 120, 0)))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("a reversed step window accepted: %v", err)
	}
}

func TestAStepBeginningBeforeTheRecordStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(ran("build", 90, 140, 50*time.Second)))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("a step beginning before the record accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "before the record's start stamp") {
		t.Fatalf("the refusal did not say which edge: %v", err)
	}
}

func TestAStepEndingAfterTheRecordStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(ran("build", 110, 260, 150*time.Second)))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("a step ending after the record accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "after the record's finish stamp") {
		t.Fatalf("the refusal did not say which edge: %v", err)
	}
}

func TestANegativeStepDurationStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(ran("build", 110, 140, -time.Second)))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("a negative step duration accepted: %v", err)
	}
}

func TestAStepReportingMoreTimeThanItsStampsAllowStopsTheSweep(t *testing.T) {
	err := checkUploadedStepWindowsWereMeasured(windowed(ran("build", 110, 140, time.Hour)))
	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("an hour measured between stamps 30s apart accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "apart") {
		t.Fatalf("the refusal did not state the span: %v", err)
	}
}

func TestTwoClocksDisagreeingSlightlyAreStillUploaded(t *testing.T) {
	// The duration comes from monotonic readings and the stamps from the wall
	// clock, so the two differ on every real run. Five seconds is the
	// allowance the store fixes for exactly this comparison.
	if err := checkUploadedStepWindowsWereMeasured(windowed(
		ran("build", 110, 140, 34*time.Second),
	)); err != nil {
		t.Fatalf("a step within the clock allowance refused: %v", err)
	}
}

func TestAStepMeasuringLessThanItsStampsIsUploaded(t *testing.T) {
	// The stamps bracket the whole step while the duration may measure the
	// child alone, so shorter is ordinary and only longer is a contradiction.
	if err := checkUploadedStepWindowsWereMeasured(windowed(
		ran("build", 110, 140, time.Millisecond),
	)); err != nil {
		t.Fatalf("a step measuring less than its span refused: %v", err)
	}
}

func TestTheAttemptsOwnDurationIsLeftToTheDoorThatDerivesIt(t *testing.T) {
	// offlineInput computes the run's DurationNS from the record's own stamps
	// and never reads Result.Duration, so refusing it here would throw away a
	// completed run over a field durable history does not file.
	entry := windowed(ran("build", 110, 140, 30*time.Second))
	entry.Result.Duration = 400 * time.Hour
	if err := checkUploadedStepWindowsWereMeasured(entry); err != nil {
		t.Fatalf("the door judged the attempt's own duration: %v", err)
	}
}

func TestARecordWithNoResultStatesNoStepWindow(t *testing.T) {
	entry := windowed()
	entry.Result = nil
	if err := checkUploadedStepWindowsWereMeasured(entry); err != nil {
		t.Fatalf("a record without a result was judged for step windows: %v", err)
	}
}

func TestARecordWithNoFinishStampStatesNoStepWindow(t *testing.T) {
	// The envelope door refuses this record ahead of here, and it says so with
	// the stamp named; this door has no envelope to judge a step against.
	entry := windowed(ran("build", 110, 140, 30*time.Second))
	entry.FinishedAt = nil
	if err := checkUploadedStepWindowsWereMeasured(entry); err != nil {
		t.Fatalf("a record without a finish stamp was judged for step windows: %v", err)
	}
}

func TestASteplessRecordStatesNoStepWindow(t *testing.T) {
	if err := checkUploadedStepWindowsWereMeasured(windowed()); err != nil {
		t.Fatalf("a stepless record refused: %v", err)
	}
}
