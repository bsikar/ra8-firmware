// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

// Which failures the run's own budget explains. A timed-out attempt that was
// reported is finished business: ending the poll loop over it would restart
// the agent after every timeout that still had a log chunk in flight. A
// failure that merely happened to coincide with the deadline is a different
// thing and still has to end the loop, so the two must not be confused.

import (
	"context"
	"errors"
	"fmt"
	"testing"
	"time"
)

func expiredContext(t *testing.T) context.Context {
	t.Helper()
	ctx, cancel := context.WithDeadline(context.Background(), time.Now().Add(-time.Hour))
	t.Cleanup(cancel)
	if !errors.Is(ctx.Err(), context.DeadlineExceeded) {
		t.Fatalf("fixture context is %v", ctx.Err())
	}
	return ctx
}

func TestSpentWithTheBudgetNamesOnlyTheDeadline(t *testing.T) {
	live, cancel := context.WithCancel(context.Background())
	defer cancel()
	cancelled, stop := context.WithCancel(context.Background())
	stop()
	wrapped := fmt.Errorf("execute format-tree-check: %w", fmt.Errorf("request failed: %w", context.DeadlineExceeded))

	for _, tc := range []struct {
		name string
		ctx  context.Context
		err  error
		want bool
	}{
		{"deadline reached and the failure wraps it", expiredContext(t), wrapped, true},
		{"deadline reached, bare", expiredContext(t), context.DeadlineExceeded, true},
		// The attempt timed out, but this failure is about something else:
		// the plane refusing the word of an attempt it has fenced away. That
		// still has to reach the caller.
		{"deadline reached, unrelated failure", expiredContext(t), ErrServerProtocol, false},
		{"deadline reached, nothing failed", expiredContext(t), nil, false},
		// A cancelled run is not a spent budget. It is the plane telling this
		// attempt to stop, and the loop is meant to end on it.
		{"cancelled rather than timed out", cancelled, context.Canceled, false},
		{"still running", live, wrapped, false},
		{"no context", nil, wrapped, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := spentWithTheBudget(tc.ctx, tc.err); got != tc.want {
				t.Fatalf("spentWithTheBudget = %v, want %v", got, tc.want)
			}
		})
	}
}

// RunOnce hands this predicate a JOIN, and errors.Is is satisfied by any one
// member of a join. So the case that matters is a real failure travelling
// beside a deadline: asking errors.Is directly would swallow it, and the loop
// would keep running after the plane had refused this attempt's word.
func TestSpentWithTheBudgetRefusesAJoinCarryingARealFailure(t *testing.T) {
	ctx := expiredContext(t)
	logDeadline := fmt.Errorf("execute format-tree-check: %w", context.DeadlineExceeded)

	if !spentWithTheBudget(ctx, errors.Join(logDeadline, nil)) {
		t.Fatal("a join of nothing but the deadline was not read as the budget")
	}
	if !spentWithTheBudget(ctx, errors.Join(logDeadline, context.DeadlineExceeded)) {
		t.Fatal("a join of two deadlines was not read as the budget")
	}
	for _, beside := range []error{ErrServerProtocol, ErrUnsafeArtifact,
		fmt.Errorf("collect artifacts: %w", ErrUnsafeAssignment)} {
		if spentWithTheBudget(ctx, errors.Join(logDeadline, beside)) {
			t.Fatalf("%v was swallowed as the budget", beside)
		}
	}
	// An empty join is not the deadline either: nothing explains nothing.
	if spentWithTheBudget(ctx, errors.Join()) {
		t.Fatal("an empty join was read as the budget")
	}
}
