//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What the plane says once a deadline has overtaken the attempt.
//
// The deadline is the one thing an agent cannot see for itself, so the
// plane answers it twice over, and the two answers are deliberately
// different. A heartbeat past the deadline is still ACCEPTED and comes
// back carrying the order to stop, because an agent that is still talking
// is an agent that can still be told. A terminal receipt past the deadline
// plus its grace is REFUSED, because by then the reaper owns the attempt
// and a late report would overwrite whatever it concluded.
//
// Neither case is waited out: the attempt's deadline is moved, which gets
// to the same state in milliseconds, and is put back afterwards so the
// following case starts from a live attempt.

func TestIntegrationWhenADeadlineOvertakesAnAttempt(t *testing.T) {
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
	beat := protocol.Heartbeat{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, Phase: "executing", HostFacts: facts}
	moveDeadline := func(t *testing.T, interval string) {
		t.Helper()
		if _, err := pool.Exec(ctx, `UPDATE task_attempts
			SET deadline_at=clock_timestamp()+`+interval+` WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
	}

	t.Run("an attempt inside its deadline is told to carry on", func(t *testing.T) {
		// The baseline. Without it, every Cancel below could just be
		// the plane telling every agent to stop.
		answer, err := st.HeartbeatAgentAttempt(ctx, cert, beat)
		if err != nil {
			t.Fatalf("a live attempt had its heartbeat refused: %v", err)
		}
		if answer.Cancel {
			t.Fatal("an attempt inside its deadline was told to stop")
		}
		if answer.AssignmentVersion != grant.AssignmentVersion || answer.FencingToken != grant.FencingToken {
			t.Fatalf("the answer came back on a different grant: %+v", answer)
		}
	})

	t.Run("an attempt past its deadline is told to stop", func(t *testing.T) {
		// Accepted, not refused: the heartbeat is the only channel the
		// plane has to reach a running agent, so it answers rather
		// than hanging up.
		moveDeadline(t, "interval '-1 minute'")
		answer, err := st.HeartbeatAgentAttempt(ctx, cert, beat)
		if err != nil {
			t.Fatalf("a heartbeat past the deadline was refused rather than answered: %v", err)
		}
		if !answer.Cancel {
			t.Fatal("an attempt past its deadline was told to carry on")
		}
		moveDeadline(t, "interval '10 minutes'")
	})

	t.Run("an attempt whose run has been cancelled is told to stop", func(t *testing.T) {
		// The second way the same flag is raised, and the one that has
		// nothing to do with time: the operator asked for the run to
		// end while this attempt was still healthy.
		// The schema keeps the two cancellation columns paired, so a
		// request has to name who made it as well as when.
		if _, err := pool.Exec(ctx, `UPDATE runs SET cancel_requested_at=clock_timestamp(),
			cancel_requested_by='integration-operator' WHERE id=$1`, run.ID); err != nil {
			t.Fatal(err)
		}
		answer, err := st.HeartbeatAgentAttempt(ctx, cert, beat)
		if err != nil {
			t.Fatalf("a heartbeat on a cancelled run was refused rather than answered: %v", err)
		}
		if !answer.Cancel {
			t.Fatal("an attempt on a cancelled run was told to carry on")
		}
		if _, err := pool.Exec(ctx, `UPDATE runs SET cancel_requested_at=NULL,
			cancel_requested_by=NULL WHERE id=$1`, run.ID); err != nil {
			t.Fatal(err)
		}
	})

	definition, found := cat.Task("format-check")
	if !found || len(definition.Steps) == 0 {
		t.Fatalf("the fixture's task has no catalog definition: found=%v", found)
	}
	started := time.Now().UTC().Add(-time.Second)
	ended := time.Now().UTC()
	empty := sha256.Sum256(nil)
	emptyDigest := hex.EncodeToString(empty[:])
	exitCode := 1
	steps := make([]protocol.StepSummary, 0, len(definition.Steps))
	for _, declared := range definition.Steps {
		steps = append(steps, protocol.StepSummary{Name: declared.Name,
			StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
			ExitCode: 1, StdoutSHA256: emptyDigest, StderrSHA256: emptyDigest})
	}
	receipt := protocol.TerminalReceipt{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Outcome: "failed", ChildExitCode: &exitCode, EvidenceComplete: false,
		StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
		Steps: steps, FinalLogSequence: 0,
		CatalogSHA256: grant.CatalogSHA256, SourceSnapshotSHA256: grant.Source.SnapshotSHA256,
		HostFactsAtStart: facts, HostFactsAtEnd: facts}

	t.Run("a receipt arriving after the evidence grace is refused", func(t *testing.T) {
		// Past the deadline AND its grace, the reaper has already
		// judged this attempt, so the report has nowhere to land.
		moveDeadline(t, "interval '-2 minutes'")
		if err := st.CompleteAgentAttempt(ctx, cert, receipt, cat); !errors.Is(err, ErrConflict) {
			t.Fatalf("a receipt past the evidence grace was accepted: %v", err)
		}
		// Nothing was written: the attempt is untouched and no step
		// rows were laid down, so the reaper's own verdict still
		// stands alone.
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "running" {
			t.Fatalf("the refused receipt moved the attempt to %q", state)
		}
		var recorded int64
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM task_steps WHERE attempt_id=$1", grant.AttemptID).Scan(&recorded); err != nil {
			t.Fatal(err)
		}
		if recorded != 0 {
			t.Fatalf("the refused receipt recorded %d step(s)", recorded)
		}
	})

	t.Run("the same receipt inside the grace is taken", func(t *testing.T) {
		// The refusal above is about the clock and nothing else: put
		// the deadline back and the identical receipt completes.
		moveDeadline(t, "interval '10 minutes'")
		if err := st.CompleteAgentAttempt(ctx, cert, receipt, cat); err != nil {
			t.Fatalf("a receipt inside the grace was refused: %v", err)
		}
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "failed" {
			t.Fatalf("the accepted receipt left the attempt at %q", state)
		}
	})
}
