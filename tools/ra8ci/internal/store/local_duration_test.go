// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"math"
	"strings"
	"testing"
	"time"
)

func localRunOver(span time.Duration, durationNS int64) LocalRunInput {
	started := time.Date(2026, 9, 26, 8, 0, 0, 0, time.UTC)
	return LocalRunInput{
		PrincipalID:        "principal",
		LocalID:            strings.Repeat("a", 32),
		PayloadSHA256:      strings.Repeat("b", 64),
		CatalogSHA256:      strings.Repeat("c", 64),
		SourceVerification: "unverified",
		Repository:         "bsikar/ra8-firmware",
		TaskName:           "unit",
		Tier:               "required",
		Scope:              "safe-local-read-only",
		DeadlineSeconds:    60,
		StartedAt:          started,
		FinishedAt:         started.Add(span),
		DurationNS:         durationNS,
		Result:             "succeeded",
	}
}

func localStep(start, end time.Time, durationNS int64) LocalStepInput {
	return LocalStepInput{
		Key:          "build",
		Ordinal:      0,
		StartedAt:    start,
		EndedAt:      end,
		DurationNS:   durationNS,
		StdoutSHA256: strings.Repeat("d", 64),
		StderrSHA256: strings.Repeat("e", 64),
	}
}

func TestADurationLongerThanItsStampsIsRefused(t *testing.T) {
	in := localRunOver(time.Millisecond, time.Hour.Nanoseconds())
	err := checkLocalRunDurations(in)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("an hour between stamps a millisecond apart must be refused, got %v", err)
	}
	if !strings.Contains(err.Error(), "local run reports") {
		t.Fatalf("the refusal must name both numbers, got %q", err)
	}
}

func TestADurationShorterThanItsStampsIsOrdinary(t *testing.T) {
	// The stamps bracket the whole attempt while the duration may measure the
	// child alone, so the rule is one-sided and this is not a contradiction.
	if err := checkLocalRunDurations(localRunOver(time.Hour, time.Second.Nanoseconds())); err != nil {
		t.Fatalf("a duration inside its span must be accepted, got %v", err)
	}
}

func TestAMeasurementOfNothingIsStillAccepted(t *testing.T) {
	// An incomplete_evidence upload may carry no measurement at all.
	if err := checkLocalRunDurations(localRunOver(time.Hour, 0)); err != nil {
		t.Fatalf("a zero duration must be accepted, got %v", err)
	}
}

func TestTheClockAllowanceIsExactlyFiveSeconds(t *testing.T) {
	span := time.Second
	for _, tc := range []struct {
		over    time.Duration
		refused bool
	}{
		{over: 0},
		{over: time.Nanosecond},
		{over: 5 * time.Second},
		{over: 5*time.Second + time.Nanosecond, refused: true},
		{over: time.Minute, refused: true},
	} {
		in := localRunOver(span, (span + tc.over).Nanoseconds())
		err := checkLocalRunDurations(in)
		if tc.refused != (err != nil) {
			t.Fatalf("%s over the span: refused=%v, got %v", tc.over, tc.refused, err)
		}
	}
}

func TestAStepDurationIsJudgedAgainstItsOwnStamps(t *testing.T) {
	// dispatch.go holds a step to the RECEIPT's total, which a step reporting
	// the whole attempt's time between two of its own adjacent stamps passes.
	in := localRunOver(time.Hour, time.Hour.Nanoseconds())
	in.Steps = []LocalStepInput{localStep(in.StartedAt, in.StartedAt.Add(time.Millisecond), time.Hour.Nanoseconds())}
	err := checkLocalRunDurations(in)
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "local step 0") {
		t.Fatalf("a step must be judged against its own stamps, got %v", err)
	}
}

func TestEveryStepIsJudgedNotOnlyTheFirst(t *testing.T) {
	in := localRunOver(time.Hour, time.Hour.Nanoseconds())
	good := localStep(in.StartedAt, in.StartedAt.Add(time.Minute), time.Minute.Nanoseconds())
	bad := localStep(in.StartedAt.Add(time.Minute), in.StartedAt.Add(2*time.Minute), time.Hour.Nanoseconds())
	bad.Key, bad.Ordinal = "test", 1
	in.Steps = []LocalStepInput{good, bad}
	if err := checkLocalRunDurations(in); !strings.Contains(err.Error(), "local step 1") {
		t.Fatalf("the second step must be judged too, got %v", err)
	}
}

func TestASpanTooWideToMeasureIsRefusedRatherThanAdmittingAnything(t *testing.T) {
	// time.Time.Sub saturates at MaxInt64, so without this a pair of absurd
	// stamps would admit every duration there is.
	in := localRunOver(time.Hour, math.MaxInt64)
	in.FinishedAt = in.StartedAt.Add(time.Duration(math.MaxInt64)).Add(time.Hour)
	err := checkLocalRunDurations(in)
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "longer than a clock can measure") {
		t.Fatalf("a saturated span must be refused, got %v", err)
	}
}

func TestTheRefusalIsWordedAsTheAgentPathWordsIt(t *testing.T) {
	// protocol/receipt_duration.go refuses the same contradiction in these
	// words; two surfaces reporting one rule differently is the drift this
	// pins against.
	in := localRunOver(time.Millisecond, time.Hour.Nanoseconds())
	err := checkLocalRunDurations(in)
	if err == nil || !strings.Contains(err.Error(), "ns between stamps") ||
		!strings.Contains(err.Error(), "ns apart") {
		t.Fatalf("unexpected wording: %v", err)
	}
}

func TestValidateLocalRunAppliesTheDurationRule(t *testing.T) {
	in := localRunOver(time.Millisecond, time.Hour.Nanoseconds())
	if err := validateLocalRun(in); !errors.Is(err, ErrInvalid) {
		t.Fatalf("ingest validation must apply the rule, got %v", err)
	}
	ok := localRunOver(time.Minute, time.Minute.Nanoseconds())
	if err := validateLocalRun(ok); err != nil {
		t.Fatalf("an honest upload must still validate, got %v", err)
	}
}
