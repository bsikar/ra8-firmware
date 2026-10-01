//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
	"time"
)

// What a failed prerequisite takes down with it.
//
// A task that cannot run because something it depended on failed is
// skipped rather than left scheduled forever, and the skip reaches the
// WHOLE subtree, not just the task immediately downstream. That recursion
// is the part worth holding in place: a chain of three means the failure
// has to travel two edges, and a one-level implementation would quietly
// strand the far end of every pipeline.
//
// The run is driven through the local attempt path rather than the agent
// path, because the descendant logic is shared by both and the local one
// says what it means without a certificate in the way.

func chainedRun(t *testing.T, keys ...string) CreateRunInput {
	t.Helper()
	in := CreateRunInput{Trigger: "integration", ActorID: "integration-operator",
		Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("a", 40),
		SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: strings.Repeat("c", 64),
		IdempotencyKey: "chain-" + mustID(t), RequestSHA256: strings.Repeat("d", 64)}
	for index, key := range keys {
		task := TaskInput{Key: key, Name: "format-check",
			Arguments: json.RawMessage(`{"argv":[]}`), Tier: "required",
			Scope: "safe-local-read-only", HostClass: "linux-vm", DeadlineSeconds: 60}
		if index > 0 {
			task.DependsOnKeys = []string{keys[index-1]}
		}
		in.Tasks = append(in.Tasks, task)
	}
	return in
}

func TestIntegrationTheWorkAFailedPrerequisiteTakesDown(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	taskStates := func(t *testing.T, runID string) map[string]string {
		t.Helper()
		rows, err := pool.Query(ctx, "SELECT task_key, state FROM tasks WHERE run_id=$1", runID)
		if err != nil {
			t.Fatal(err)
		}
		defer rows.Close()
		states := map[string]string{}
		for rows.Next() {
			var key, state string
			if err := rows.Scan(&key, &state); err != nil {
				t.Fatal(err)
			}
			states[key] = state
		}
		if err := rows.Err(); err != nil {
			t.Fatal(err)
		}
		return states
	}
	byKey := func(run Run) map[string]string {
		ids := map[string]string{}
		for _, task := range run.Tasks {
			ids[task.Key] = task.ID
		}
		return ids
	}

	t.Run("a failure travels the whole chain, not one edge", func(t *testing.T) {
		run, err := st.CreateRun(ctx, chainedRun(t, "first", "second", "third"))
		if err != nil {
			t.Fatal(err)
		}
		ids := byKey(run)
		attempt, err := st.StartAttempt(ctx, testStart(ids["first"]))
		if err != nil {
			t.Fatal(err)
		}
		exitCode := 1
		if err := st.FinishAttempt(ctx, FinishAttemptInput{AttemptID: attempt.ID,
			ActorID: "tester", Result: "failed", ChildExitCode: &exitCode,
			EvidenceComplete: true}); err != nil {
			t.Fatal(err)
		}
		states := taskStates(t, run.ID)
		if states["first"] != "failed" {
			t.Fatalf("the failed task reads %q", states["first"])
		}
		// "third" never touched the failure directly: it depends on
		// "second", which depends on "first". Reaching it takes the
		// recursive half of the descendant query.
		for _, key := range []string{"second", "third"} {
			if states[key] != "skipped" {
				t.Fatalf("task %q reads %q, not skipped", key, states[key])
			}
		}
		var reasons []string
		rows, err := pool.Query(ctx, `SELECT skip_reason FROM tasks
			WHERE run_id=$1 AND state='skipped'`, run.ID)
		if err != nil {
			t.Fatal(err)
		}
		for rows.Next() {
			var reason string
			if err := rows.Scan(&reason); err != nil {
				rows.Close()
				t.Fatal(err)
			}
			reasons = append(reasons, reason)
		}
		rows.Close()
		if len(reasons) != 2 {
			t.Fatalf("%d task(s) carry a skip reason", len(reasons))
		}
		for _, reason := range reasons {
			if reason != "prerequisite_failed" {
				t.Fatalf("a skipped task explains itself as %q", reason)
			}
		}
		// Each skip is on the record twice over: the operator-facing
		// audit row names the prerequisite that did it, and the run's
		// event stream carries one entry per skipped task.
		var audited int64
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM audit
			WHERE action='task.skipped' AND correlation_run_id=$1
			AND previous_state='scheduled' AND new_state='skipped'
			AND reason->>'failed_prerequisite'=$2`, run.ID, ids["first"]).Scan(&audited); err != nil {
			t.Fatal(err)
		}
		if audited != 2 {
			t.Fatalf("%d skip(s) were audited against the failed prerequisite", audited)
		}
		var events int64
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM run_events
			WHERE run_id=$1 AND kind='task.skipped'
			AND data->>'reason'='prerequisite_failed'`, run.ID).Scan(&events); err != nil {
			t.Fatal(err)
		}
		if events != 2 {
			t.Fatalf("the run carries %d skip event(s)", events)
		}
		// With nothing left that can run, the run itself is closed out
		// in the same transaction.
		finished, err := st.GetRun(ctx, run.ID)
		if err != nil {
			t.Fatal(err)
		}
		if finished.State != "terminal" {
			t.Fatalf("the run is still %q with no runnable task left", finished.State)
		}
	})

	t.Run("a prerequisite that succeeds takes nothing down", func(t *testing.T) {
		// The mirror image, and the reason the case above means
		// anything: the same shape of run, finished the other way,
		// leaves the downstream work alone and the run open.
		run, err := st.CreateRun(ctx, chainedRun(t, "first", "second"))
		if err != nil {
			t.Fatal(err)
		}
		ids := byKey(run)
		attempt, err := st.StartAttempt(ctx, testStart(ids["first"]))
		if err != nil {
			t.Fatal(err)
		}
		zero := 0
		if err := st.FinishAttempt(ctx, FinishAttemptInput{AttemptID: attempt.ID,
			ActorID: "tester", Result: "succeeded", ChildExitCode: &zero,
			EvidenceComplete: true}); err != nil {
			t.Fatal(err)
		}
		states := taskStates(t, run.ID)
		if states["first"] != "succeeded" {
			t.Fatalf("the finished task reads %q", states["first"])
		}
		if states["second"] != "scheduled" {
			t.Fatalf("the downstream task reads %q, not scheduled", states["second"])
		}
		var skips int64
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM run_events
			WHERE run_id=$1 AND kind='task.skipped'`, run.ID).Scan(&skips); err != nil {
			t.Fatal(err)
		}
		if skips != 0 {
			t.Fatalf("a successful prerequisite raised %d skip event(s)", skips)
		}
		open, err := st.GetRun(ctx, run.ID)
		if err != nil {
			t.Fatal(err)
		}
		if open.State == "terminal" {
			t.Fatal("the run was closed with work still to do")
		}
	})
}
