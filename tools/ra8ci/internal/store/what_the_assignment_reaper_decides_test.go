//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What the assignment reaper decides, and what it writes down.
//
// The reaper is the only writer that moves work an agent still believes it
// holds, so its grace period and its paper trail are both load bearing. A
// run cannot stay blocked by a crashed agent, but neither can an agent that
// is merely slow have its work taken away a second early.
//
// The reaper's retry edge is deliberately not exercised here: the catalog
// refuses any task whose Retry.MaxAttempts is not 1 (catalog.go's v1
// behaviour rule), and attempt numbers start at 1, so retryable() cannot
// be true in v1 and every reaped task is lost rather than requeued. That
// edge stays in the plane for a catalog that allows retries later.

func TestIntegrationWhatTheAssignmentReaperDecides(t *testing.T) {
	st, pool, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatal(err)
	}
	var taskID string
	if err := pool.QueryRow(ctx, "SELECT id::text FROM tasks WHERE run_id=$1", run.ID).Scan(&taskID); err != nil {
		t.Fatal(err)
	}
	// The deadline is moved rather than waited out: the reaper's grace is
	// 65 seconds, so 30 seconds past the deadline is inside it and two
	// minutes past is beyond it.
	expire := func(t *testing.T, interval string) {
		t.Helper()
		if _, err := pool.Exec(ctx, `UPDATE task_attempts
			SET deadline_at=clock_timestamp()-`+interval+` WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
	}
	attemptState := func(t *testing.T) string {
		t.Helper()
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		return state
	}

	t.Run("an attempt still inside the reaper grace is left alone", func(t *testing.T) {
		// Past its deadline but not past the grace, which is the window
		// a healthy agent uses to shut its child down and finish
		// delivering logs.
		expire(t, "interval '30 seconds'")
		reaped, err := st.ReapAgentAssignments(ctx, cat, 100)
		if err != nil {
			t.Fatalf("the reaper failed over an attempt inside its grace: %v", err)
		}
		if reaped != 0 {
			t.Fatalf("the reaper took %d attempt(s) inside the grace", reaped)
		}
		if state := attemptState(t); state != "running" {
			t.Fatalf("an attempt inside its grace was moved to %q", state)
		}
	})

	t.Run("a task that moved under the reaper is reported, not overwritten", func(t *testing.T) {
		// The attempt is genuinely expired, but the task it belongs to
		// has already reached an outcome, so the edge the reaper is
		// about to write is not one the task machine allows. That is a
		// race worth reporting: the write is dropped whole rather than
		// half applied.
		expire(t, "interval '2 minutes'")
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='succeeded',
			ended_at=clock_timestamp(), version=version+1 WHERE id=$1`, taskID); err != nil {
			t.Fatal(err)
		}
		reaped, err := st.ReapAgentAssignments(ctx, cat, 100)
		if err == nil {
			t.Fatal("the reaper wrote over a task that had already finished")
		}
		t.Logf("the reaper refused the illegal edge: %v", err)
		if reaped != 0 {
			t.Fatalf("the reaper counted %d attempt(s) it could not move", reaped)
		}
		if state := attemptState(t); state != "running" {
			t.Fatalf("the refused reap still moved the attempt to %q", state)
		}
		if _, err := pool.Exec(ctx, `UPDATE tasks SET state='running',
			ended_at=NULL, version=version+1 WHERE id=$1`, taskID); err != nil {
			t.Fatal(err)
		}
	})

	t.Run("an abandoned attempt is fenced and the reason is written down", func(t *testing.T) {
		reaped, err := st.ReapAgentAssignments(ctx, cat, 100)
		if err != nil || reaped != 1 {
			t.Fatalf("the reaper took %d attempt(s): %v", reaped, err)
		}
		// The attempt carries why it ended, which is what tells an
		// operator the agent went quiet rather than reported a failure.
		var state, reason string
		var evidenceComplete bool
		var ended *time.Time
		if err := pool.QueryRow(ctx, `SELECT state, result_reason, evidence_complete, ended_at
			FROM task_attempts WHERE id=$1`, grant.AttemptID).Scan(&state, &reason, &evidenceComplete, &ended); err != nil {
			t.Fatal(err)
		}
		if state != "lost" || reason != "assignment_deadline_without_terminal" {
			t.Fatalf("the fenced attempt reads %q / %q", state, reason)
		}
		if evidenceComplete {
			t.Fatal("an attempt that never reported was marked evidence complete")
		}
		if ended == nil {
			t.Fatal("the fenced attempt has no end time")
		}
		// The task follows it, and in v1 that is always lost: no task
		// may declare more than one attempt.
		var taskState string
		if err := pool.QueryRow(ctx, "SELECT state FROM tasks WHERE id=$1", taskID).Scan(&taskState); err != nil {
			t.Fatal(err)
		}
		if taskState != "lost" {
			t.Fatalf("the reaped task reads %q, not lost", taskState)
		}
		// The audit row is the operator-facing record, so its states
		// and its reason both matter.
		var previous, next, outcome, auditReason, retry string
		if err := pool.QueryRow(ctx, `SELECT previous_state, new_state, outcome,
			reason->>'reason', reason->>'retry' FROM audit
			WHERE action='task.assignment.killed' AND target_id=$1`, taskID).Scan(
			&previous, &next, &outcome, &auditReason, &retry); err != nil {
			t.Fatal(err)
		}
		if previous != "running" || next != "lost" || outcome != "ok" {
			t.Fatalf("the audit row reads %q -> %q (%q)", previous, next, outcome)
		}
		if auditReason != "assignment_deadline_without_terminal" || retry != "false" {
			t.Fatalf("the audit row explains it as %q, retry=%q", auditReason, retry)
		}
		// And the run's own event stream carries it too, which is what
		// a client watching the run actually sees.
		var events int64
		if err := pool.QueryRow(ctx, `SELECT count(*) FROM run_events
			WHERE run_id=$1 AND kind='assignment.reaped' AND data->>'attempt_id'=$2`,
			run.ID, grant.AttemptID).Scan(&events); err != nil {
			t.Fatal(err)
		}
		if events != 1 {
			t.Fatalf("the run carries %d reaping event(s)", events)
		}
	})

	t.Run("a reaped attempt is not reaped twice", func(t *testing.T) {
		// The attempt is still past its deadline, so only its state
		// keeps it out of the candidate set. A second sweep has to be
		// a no-op or the run's event stream would fill with repeats.
		reaped, err := st.ReapAgentAssignments(ctx, cat, 100)
		if err != nil {
			t.Fatalf("the second sweep failed: %v", err)
		}
		if reaped != 0 {
			t.Fatalf("the second sweep took %d already-fenced attempt(s)", reaped)
		}
	})
}
