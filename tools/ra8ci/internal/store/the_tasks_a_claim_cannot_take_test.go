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

// The tasks a local claim cannot take.
//
// A claim is the moment one runner takes ownership of a piece of work, so
// the three ways it can arrive against a task it may not have each answer
// differently: a task that does not exist, a task somebody else is already
// running, and a task whose scope does not permit the board the caller is
// trying to bring with it.

func TestIntegrationAClaimCannotTakeATaskThatDoesNotExist(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	start := testStart(mustID(t))
	if _, err := s.StartAttempt(ctx, start); !errors.Is(err, ErrNotFound) {
		t.Fatalf("a claim on an unknown task was not reported missing: %v", err)
	}
}

func TestIntegrationATaskAlreadyRunningIsNotClaimedTwice(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	run, err := s.CreateRun(ctx, testRun())
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID)); err != nil {
		t.Fatal(err)
	}

	// Two runners racing for the same scheduled task is the case this
	// guard exists for. The second one is refused on the task's own
	// state rather than quietly opening a second attempt, so the work
	// never runs twice under one task.
	_, err = s.StartAttempt(ctx, testStart(run.Tasks[0].ID))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a running task was claimed a second time: %v", err)
	}

	var attempts int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM task_attempts WHERE task_id=$1", run.Tasks[0].ID).Scan(&attempts); err != nil {
		t.Fatal(err)
	}
	if attempts != 1 {
		t.Fatalf("the refused claim left %d attempts on the task", attempts)
	}
}

func TestIntegrationALocalTaskCannotClaimABoardLease(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	run, err := s.CreateRun(ctx, testRun())
	if err != nil {
		t.Fatal(err)
	}

	// Hardware is reserved by scope, which is written into the task when
	// the run is created and cannot be talked into existence at claim
	// time. A caller that arrives with a board lease for work that was
	// never scoped to hardware is refused, so a lease is never attached
	// to an attempt the catalog did not intend to run on a board.
	start := testStart(run.Tasks[0].ID)
	start.BoardLeaseID = mustID(t)
	_, err = s.StartAttempt(ctx, start)
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a non-HIL task claimed a board lease: %v", err)
	}
	if !strings.Contains(err.Error(), "non-HIL attempt cannot claim board timing or a board lease") {
		t.Fatalf("the refusal did not name what it refused: %v", err)
	}

	// The task is untouched and still claimable the ordinary way.
	if _, err := s.StartAttempt(ctx, testStart(run.Tasks[0].ID)); err != nil {
		t.Fatalf("the refused claim left the task unclaimable: %v", err)
	}
}
