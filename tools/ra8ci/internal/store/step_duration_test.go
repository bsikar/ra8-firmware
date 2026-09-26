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

func recordedStep(span time.Duration, durationNS int64) StepInput {
	started := time.Date(2026, 9, 26, 13, 0, 0, 0, time.UTC)
	return StepInput{
		AttemptID:  "0192abcd-1234-7abc-89ab-0123456789ab",
		ActorID:    "agent",
		Key:        "observe",
		Ordinal:    0,
		Phase:      "hil_observe",
		StartedAt:  started,
		EndedAt:    started.Add(span),
		DurationNS: durationNS,
		State:      "succeeded",
	}
}

func TestARecordedStepLongerThanItsStampsIsRefused(t *testing.T) {
	err := checkRecordedStepDuration(recordedStep(time.Millisecond, time.Hour.Nanoseconds()))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("an hour between stamps a millisecond apart must be refused, got %v", err)
	}
	if !strings.Contains(err.Error(), "step observe reports") {
		t.Fatalf("the refusal must name the step and both numbers, got %q", err)
	}
}

func TestARecordedStepInsideItsStampsIsOrdinary(t *testing.T) {
	// The stamps bracket the whole step while the duration may measure the
	// child alone, so the rule is one-sided and this is not a contradiction.
	for _, durationNS := range []int64{0, time.Second.Nanoseconds(), time.Minute.Nanoseconds()} {
		if err := checkRecordedStepDuration(recordedStep(time.Hour, durationNS)); err != nil {
			t.Fatalf("duration %dns inside an hour must be accepted, got %v", durationNS, err)
		}
	}
}

func TestTheRecordedStepAllowanceIsTheOneThisPackageAlreadyStates(t *testing.T) {
	span := time.Second
	for _, tc := range []struct {
		over    time.Duration
		refused bool
	}{
		{over: 0},
		{over: time.Nanosecond},
		{over: localClockDisagreement},
		{over: localClockDisagreement + time.Nanosecond, refused: true},
		{over: time.Minute, refused: true},
	} {
		err := checkRecordedStepDuration(recordedStep(span, (span + tc.over).Nanoseconds()))
		if tc.refused && !errors.Is(err, ErrInvalid) {
			t.Fatalf("%v over the span must be refused, got %v", tc.over, err)
		}
		if !tc.refused && err != nil {
			t.Fatalf("%v over the span is inside the allowance, got %v", tc.over, err)
		}
	}
}

func TestARecordedStepAndALocalStepAgreeOnTheSameShape(t *testing.T) {
	// The two doors state one rule. A shape either door refuses must be
	// refused by both, or durable history depends on which surface wrote it.
	started := time.Date(2026, 9, 26, 13, 0, 0, 0, time.UTC)
	for _, span := range []time.Duration{0, time.Millisecond, time.Second, time.Hour} {
		for _, durationNS := range []int64{0, time.Millisecond.Nanoseconds(), time.Second.Nanoseconds(), time.Hour.Nanoseconds()} {
			step := recordedStep(span, durationNS)
			local := localStep(started, started.Add(span), durationNS)
			stepErr := checkRecordedStepDuration(step)
			localErr := checkLocalDurationFitsStamps("local step 0", local.DurationNS, local.StartedAt, local.EndedAt)
			if (stepErr == nil) != (localErr == nil) {
				t.Fatalf("span %v duration %dns: recorded step said %v, local step said %v", span, durationNS, stepErr, localErr)
			}
		}
	}
}

func TestAStepSpanTooWideToMeasureIsRefused(t *testing.T) {
	// time.Time.Sub saturates at MaxInt64, so an absurd span would otherwise
	// admit any duration whatsoever.
	in := recordedStep(0, time.Hour.Nanoseconds())
	in.StartedAt = time.Unix(0, math.MinInt64)
	in.EndedAt = time.Unix(0, math.MaxInt64).Add(time.Hour)
	if err := checkRecordedStepDuration(in); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a span longer than a clock can measure must be refused, got %v", err)
	}
}

func TestRecordStepRefusesADurationItsStampsCannotHold(t *testing.T) {
	// The wiring test: RecordStep reaches the rule before it reaches the
	// pool, so a contradictory duration never opens a transaction.
	s := &Store{}
	err := s.RecordStep(t.Context(), recordedStep(time.Millisecond, time.Hour.Nanoseconds()))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("RecordStep must refuse the contradiction, got %v", err)
	}
	if !strings.Contains(err.Error(), "step observe reports") {
		t.Fatalf("RecordStep must return this rule's refusal, not the generic one, got %q", err)
	}
}

func TestRecordStepStillRefusesReversedStampsAsMetadata(t *testing.T) {
	// The older metadata rule owns the reversed pair; this one must not
	// take it over, or the caller loses the reason it was refused.
	s := &Store{}
	in := recordedStep(time.Minute, time.Second.Nanoseconds())
	in.StartedAt, in.EndedAt = in.EndedAt, in.StartedAt
	err := s.RecordStep(t.Context(), in)
	if !errors.Is(err, ErrInvalid) || !strings.Contains(err.Error(), "step metadata") {
		t.Fatalf("reversed stamps stay the metadata refusal, got %v", err)
	}
}
