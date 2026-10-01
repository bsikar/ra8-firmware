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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// The authority an acknowledgment has to still hold.
//
// An assignment is handed out once and acknowledged later, and the gap
// between the two is where authority goes stale. The fencing token already
// covers one agent overtaking another. These are the other four ways an
// acknowledgment can arrive from an agent that is no longer entitled to the
// work it is about to start, each refused differently because the agent on
// the other end has to know whether to retry, re-register, or stop.

func TestIntegrationAnAcknowledgmentMustStillHoldItsAuthority(t *testing.T) {
	st, pool, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	trustedCommit := strings.Repeat("a", 40)

	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, trustedCommit)
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	sound := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}

	t.Run("an assignment this plane never issued", func(t *testing.T) {
		// Both identifiers are well formed and the fence is sound; the
		// pair simply names no grant. Missing, not invalid: there is
		// nothing in the request for the agent to correct.
		unknown := sound
		unknown.AssignmentID = mustID(t)
		unknown.AttemptID = mustID(t)
		if err := st.AcknowledgeAgentAssignment(ctx, cert, unknown); !errors.Is(err, ErrNotFound) {
			t.Fatalf("an unknown assignment was not reported missing: %v", err)
		}
	})

	t.Run("a host that is not the one the agent registered", func(t *testing.T) {
		// The grant was sized for the agent that registered, so an
		// acknowledgment describing a different operating system is
		// not the agent the plane dispatched to, whatever the fence
		// says. Denied rather than conflicted: this is an identity
		// answer, not a race. The facts stay internally consistent
		// (windows reports its own load kind) so the refusal is the
		// plane's comparison against the registered agent and not the
		// schema refusing a malformed acknowledgment on the way in.
		moved := sound
		moved.HostFacts.OS = "windows"
		moved.HostFacts.LoadKind = "cpu_busy_equivalent"
		if err := st.AcknowledgeAgentAssignment(ctx, cert, moved); !errors.Is(err, ErrDenied) {
			t.Fatalf("an acknowledgment from a different host was accepted: %v", err)
		}
	})

	t.Run("a different source or catalog than the grant named", func(t *testing.T) {
		// The whole point of the acknowledgment is the agent proving
		// it verified the exact source and catalog it was sent. A
		// mismatch means the two sides disagree about what is about to
		// run, which is a conflict, and the work must not start.
		for name, spoil := range map[string]func(*protocol.Ack){
			"a catalog the grant did not name":  func(a *protocol.Ack) { a.CatalogSHA256 = strings.Repeat("c", 64) },
			"a snapshot the grant did not name": func(a *protocol.Ack) { a.SourceSnapshotSHA256 = strings.Repeat("d", 64) },
		} {
			disagreeing := sound
			spoil(&disagreeing)
			if err := st.AcknowledgeAgentAssignment(ctx, cert, disagreeing); !errors.Is(err, ErrConflict) {
				t.Fatalf("accepted %s: %v", name, err)
			}
		}
	})

	t.Run("an agent whose grant was withdrawn after the claim", func(t *testing.T) {
		// Authority is re-read at acknowledgment, not cached from the
		// claim. Revoking an agent's grant has to stop work that was
		// already handed out, otherwise withdrawing access would only
		// take effect for assignments nobody had issued yet.
		if _, err := pool.Exec(ctx, "DELETE FROM api_grants WHERE repository=$1", run.Repository); err != nil {
			t.Fatal(err)
		}
		if err := st.AcknowledgeAgentAssignment(ctx, cert, sound); !errors.Is(err, ErrDenied) {
			t.Fatalf("an agent acknowledged after its grant was withdrawn: %v", err)
		}

		// The attempt never left the state it was issued in, so the
		// reaper still owns it rather than it sitting half-started.
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "issued" {
			t.Fatalf("the refused acknowledgment left the attempt at %q", state)
		}
	})
}
