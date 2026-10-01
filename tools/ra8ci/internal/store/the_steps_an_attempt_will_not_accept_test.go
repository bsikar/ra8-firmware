//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// The steps a recorded attempt will not accept.
//
// Steps are the inside of an attempt: the phases a runner went through and
// what each one cost. They are written one at a time while the work runs, so
// every one of them arrives against an attempt that may have moved on since
// the runner last looked. The refusals are what keep the step history
// honest, because a step accepted after the fact would describe work nobody
// can place in time.

// stepOn builds a step whose stated duration agrees with its own stamps,
// which the plane checks before it will record one.
func stepOn(attemptID, key string, ordinal int) StepInput {
	ended := time.Now().UTC()
	started := ended.Add(-5 * time.Millisecond)
	zero := 0
	return StepInput{
		AttemptID: attemptID, ActorID: "runner", Key: key, Ordinal: ordinal,
		Phase: "execute", StartedAt: started, EndedAt: ended,
		DurationNS: ended.Sub(started).Nanoseconds(), State: "succeeded", ChildExitCode: &zero,
	}
}

func TestIntegrationAStepCannotBeRecordedAgainstAnAttemptNobodyIssued(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Well formed, and about an attempt this plane never opened. Missing
	// rather than invalid: the caller has nothing to fix in the request.
	if err := s.RecordStep(ctx, stepOn(mustID(t), "execute", 0)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("a step against an unknown attempt was not reported missing: %v", err)
	}
}

func TestIntegrationAFinishedAttemptAcceptsNoFurtherSteps(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	run, err := s.CreateRun(ctx, testRun())
	if err != nil {
		t.Fatal(err)
	}
	attempt, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	if err := s.RecordStep(ctx, stepOn(attempt.ID, "execute", 0)); err != nil {
		t.Fatal(err)
	}
	zero := 0
	if err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: attempt.ID, ActorID: "runner", Result: "succeeded",
		ChildExitCode: &zero, EvidenceComplete: true,
	}); err != nil {
		t.Fatal(err)
	}

	// A late step from a runner that has already reported its outcome
	// would change the inside of a recorded attempt. The refusal names
	// the state it found, and the attempt keeps exactly the one step it
	// had while it was running.
	err = s.RecordStep(ctx, stepOn(attempt.ID, "cleanup", 1))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a finished attempt accepted another step: %v", err)
	}
	if !strings.Contains(err.Error(), "attempt is succeeded") {
		t.Fatalf("the refusal did not name the state it found: %v", err)
	}
	var steps int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM task_steps WHERE attempt_id=$1", attempt.ID).Scan(&steps); err != nil {
		t.Fatal(err)
	}
	if steps != 1 {
		t.Fatalf("the finished attempt holds %d steps", steps)
	}
}

func TestIntegrationAStepIsRecordedOnlyOncePerKeyAndOrdinal(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	run, err := s.CreateRun(ctx, testRun())
	if err != nil {
		t.Fatal(err)
	}
	attempt, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if err != nil {
		t.Fatal(err)
	}
	if err := s.RecordStep(ctx, stepOn(attempt.ID, "execute", 0)); err != nil {
		t.Fatal(err)
	}

	// The step history is identified two ways at once, and a runner that
	// retries a report must not be able to double either one. The key is
	// how a later reader names a step; the ordinal is how it orders them.
	// A collision on the ordinal is the subtler of the two, because the
	// step looks new and would silently land beside an existing one in
	// the same position.
	for _, collision := range []struct {
		name string
		in   StepInput
	}{
		{"the same step key again", stepOn(attempt.ID, "execute", 7)},
		{"a new key in a position already taken", stepOn(attempt.ID, "cleanup", 0)},
	} {
		t.Run(collision.name, func(t *testing.T) {
			if err := s.RecordStep(ctx, collision.in); !errors.Is(err, ErrConflict) {
				t.Fatalf("recorded %s: %v", collision.name, err)
			}
		})
	}

	// Both refusals rolled back, and a genuinely new step still records.
	if err := s.RecordStep(ctx, stepOn(attempt.ID, "cleanup", 1)); err != nil {
		t.Fatalf("a distinct step was refused after the collisions: %v", err)
	}
	var steps int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM task_steps WHERE attempt_id=$1", attempt.ID).Scan(&steps); err != nil {
		t.Fatal(err)
	}
	if steps != 2 {
		t.Fatalf("the attempt holds %d steps, not the two that were accepted", steps)
	}
}
