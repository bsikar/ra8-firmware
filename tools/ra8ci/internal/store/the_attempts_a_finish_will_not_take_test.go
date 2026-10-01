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

// The attempts a finish will not take, and the run a timeout leaves behind.
//
// FinishAttempt is the write that turns running work into a recorded outcome.
// Everything downstream of it reads the row it leaves: the run summary, the
// skip of dependent work, the evidence accounting. So the three ways the
// write can arrive against a world that has already moved on each have to
// answer differently, because an operator reading the refusal is trying to
// work out whether to retry, to look elsewhere, or to stop.

func TestIntegrationAFinishCannotTakeAnAttemptItCannotFind(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Well formed and perfectly plausible, just not an attempt this plane
	// ever issued. That is missing, not invalid, and the difference is
	// what tells a caller whether to fix the request or look elsewhere.
	err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: mustID(t), ActorID: "runner", Result: "failed", EvidenceComplete: true,
	})
	if !errors.Is(err, ErrNotFound) {
		t.Fatalf("an unknown attempt was not reported missing: %v", err)
	}
}

func TestIntegrationAnAttemptIsFinishedOnlyOnce(t *testing.T) {
	s, _ := integrationStore(t)
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
	zero := 0
	finish := FinishAttemptInput{
		AttemptID: attempt.ID, ActorID: "runner", Result: "succeeded",
		ChildExitCode: &zero, EvidenceComplete: true,
	}
	if err := s.FinishAttempt(ctx, finish); err != nil {
		t.Fatal(err)
	}

	// A runner that retries its own report, or two runners that both
	// believe they hold the attempt, must not overwrite a recorded
	// outcome. The refusal names the state it found, so the second caller
	// can see the work was already accounted for rather than lost.
	err = s.FinishAttempt(ctx, finish)
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("an attempt was finished twice: %v", err)
	}
	if !strings.Contains(err.Error(), "attempt is succeeded") {
		t.Fatalf("the refusal did not name the state it found: %v", err)
	}
}

func TestIntegrationAFinishRefusesAnAttemptWhoseTaskHasMovedOn(t *testing.T) {
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

	// The attempt is still running, but its task has been taken out from
	// under it. The attempt row is judged first and would have been
	// written; the task check is what stops the whole transaction, so the
	// recorded outcome never contradicts the task that owns it.
	if _, err := pool.Exec(ctx, `UPDATE tasks SET state='cancelled', ended_at=clock_timestamp(),
		version=version+1 WHERE id=$1`, run.Tasks[0].ID); err != nil {
		t.Fatal(err)
	}
	zero := 0
	err = s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: attempt.ID, ActorID: "runner", Result: "succeeded",
		ChildExitCode: &zero, EvidenceComplete: true,
	})
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a finish was recorded against a task that had moved on: %v", err)
	}
	if !strings.Contains(err.Error(), "task is cancelled") {
		t.Fatalf("the refusal did not name the task state it found: %v", err)
	}

	// The refusal rolled back the attempt write it had already made.
	var state string
	if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", attempt.ID).Scan(&state); err != nil {
		t.Fatal(err)
	}
	if state != "running" {
		t.Fatalf("the refused finish left the attempt at %q", state)
	}
}

func TestIntegrationARunThatRanOutOfTimeSaysSo(t *testing.T) {
	s, _ := integrationStore(t)
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

	// A timeout is not a plain failure, and the run summary keeps the
	// difference: a reader triaging a red run needs to know whether the
	// work decided it had failed or never got to decide at all.
	if err := s.FinishAttempt(ctx, FinishAttemptInput{
		AttemptID: attempt.ID, ActorID: "runner", Result: "timed_out",
		HitDeadline: true, EvidenceComplete: true, Reason: "deadline reached",
	}); err != nil {
		t.Fatal(err)
	}

	finished, err := s.GetRun(ctx, run.ID)
	if err != nil {
		t.Fatal(err)
	}
	if finished.State != "terminal" || finished.ExecutionResult != "timed_out" {
		t.Fatalf("a timed-out run was summarized as %q/%q", finished.State, finished.ExecutionResult)
	}
	if finished.EvidenceState != "complete" {
		t.Fatalf("complete evidence was reported as %q", finished.EvidenceState)
	}
	for _, task := range finished.Tasks {
		switch task.Key {
		case "format":
			if task.State != "timed_out" {
				t.Fatalf("the timed-out task is %q", task.State)
			}
		case "test":
			if task.State != "skipped" {
				t.Fatalf("work behind a timeout is %q, not skipped", task.State)
			}
		}
	}
}
