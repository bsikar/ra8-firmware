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

// The receipts a terminal report will not take.
//
// A receipt is where an agent states what it ran, and the plane does not
// take its word for it. The protocol validator already judges a receipt on
// its own internal consistency. Everything below is what only the plane can
// know: whether the steps match the task definition in the catalog, and
// whether the time claimed fits the deadline the attempt was issued under.
// Each is refused as a conflict, because the receipt is well formed and the
// disagreement is with the record rather than with the request.

func TestIntegrationTheReceiptsATerminalReportWillNotTake(t *testing.T) {
	st, pool, cert, cat, _, facts := dispatchFixture(t)
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
	var state string
	if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
		t.Fatal(err)
	}
	if state != "running" {
		t.Fatalf("the acknowledged attempt is at %q, not running", state)
	}

	// The step names come from the catalog rather than from a literal, so
	// this test keeps agreeing with the definition if the task's steps are
	// ever renamed or re-ordered.
	definition, found := cat.Task("format-check")
	if !found || len(definition.Steps) == 0 {
		t.Fatalf("the fixture's task has no catalog definition: found=%v", found)
	}
	t.Logf("the catalog declares %d step(s) for format-check", len(definition.Steps))
	started := time.Now().UTC().Add(-time.Second)
	ended := time.Now().UTC()
	empty := sha256.Sum256(nil)
	emptyDigest := hex.EncodeToString(empty[:])
	failed := 1

	// A failed outcome is the honest base for these: it tolerates a step
	// that did not succeed, so each refusal below is the plane's own
	// judgment and not the validator objecting that the receipt disagrees
	// with itself.
	stepsFromDefinition := func() []protocol.StepSummary {
		steps := make([]protocol.StepSummary, 0, len(definition.Steps))
		for _, declared := range definition.Steps {
			steps = append(steps, protocol.StepSummary{Name: declared.Name,
				StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
				ExitCode: 1, StdoutSHA256: emptyDigest, StderrSHA256: emptyDigest})
		}
		return steps
	}
	sound := func() protocol.TerminalReceipt {
		return protocol.TerminalReceipt{SchemaVersion: protocol.Version,
			AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
			AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
			Outcome: "failed", ChildExitCode: &failed, EvidenceComplete: false,
			StartedAt: started, EndedAt: ended, DurationNS: ended.Sub(started).Nanoseconds(),
			Steps: stepsFromDefinition(), FinalLogSequence: 0,
			CatalogSHA256: grant.CatalogSHA256, SourceSnapshotSHA256: grant.Source.SnapshotSHA256,
			HostFactsAtStart: facts, HostFactsAtEnd: facts}
	}

	// WHAT IS NOT HERE, AND WHY. Several of the plane's own conflict arms
	// cannot be reached through a well formed receipt, because the protocol
	// validator refuses the same receipt earlier and more cheaply: more
	// steps than were run, a step whose window falls outside the attempt's,
	// a duplicated step name, and an empty step list. Those arms stay in
	// the plane as defence behind the validator, and the cases below are
	// the ones where the plane is genuinely the only judge: they need the
	// catalog definition or the attempt's deadline, neither of which the
	// validator can see.

	t.Run("a host that is not the one the agent registered", func(t *testing.T) {
		// Both snapshots move together, because the validator refuses a
		// receipt whose two readings name different machines before the
		// plane ever compares either with the registered agent.
		moved := sound()
		for _, side := range []*protocol.HostFacts{&moved.HostFactsAtStart, &moved.HostFactsAtEnd} {
			side.OS = "windows"
			side.LoadKind = "cpu_busy_equivalent"
		}
		if err := st.CompleteAgentAttempt(ctx, cert, moved, cat); !errors.Is(err, ErrDenied) {
			t.Fatalf("a receipt from a different host was accepted: %v", err)
		}
	})

	t.Run("more time than the deadline allowed", func(t *testing.T) {
		// The deadline belongs to the attempt, not to the receipt, so
		// this is a refusal only the plane can reach. The receipt is
		// kept internally consistent on purpose: stamps, duration and
		// steps all agree on a two hour run, so the validator has
		// nothing to object to and the 900 second deadline plus its
		// evidence grace is the only thing left to refuse it.
		overrun := sound()
		overrun.StartedAt = ended.Add(-2 * time.Hour)
		overrun.DurationNS = overrun.EndedAt.Sub(overrun.StartedAt).Nanoseconds()
		for i := range overrun.Steps {
			overrun.Steps[i].StartedAt = overrun.StartedAt
			overrun.Steps[i].EndedAt = overrun.EndedAt
			overrun.Steps[i].DurationNS = overrun.DurationNS
		}
		if err := st.CompleteAgentAttempt(ctx, cert, overrun, cat); !errors.Is(err, ErrConflict) {
			t.Fatalf("a receipt reporting more time than the deadline allowed was accepted: %v", err)
		}
	})

	t.Run("the attempt is still running after every refusal", func(t *testing.T) {
		// None of the above may half-finish the attempt: a refused
		// receipt has to leave the row exactly as the agent found it,
		// or a retry would be judged against a state it never caused.
		var after string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&after); err != nil {
			t.Fatal(err)
		}
		if after != "running" {
			t.Fatalf("a refused receipt moved the attempt to %q", after)
		}
		var steps int
		if err := pool.QueryRow(ctx, "SELECT count(*) FROM task_steps WHERE attempt_id=$1", grant.AttemptID).Scan(&steps); err != nil {
			t.Fatal(err)
		}
		if steps != 0 {
			t.Fatalf("a refused receipt recorded %d steps", steps)
		}
	})
}
