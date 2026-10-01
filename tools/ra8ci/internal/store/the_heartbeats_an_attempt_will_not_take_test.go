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

// The heartbeats an attempt will not take.
//
// A heartbeat is the one message an agent sends repeatedly, so it is also
// the one most likely to arrive after the thing it describes has moved on.
// Each refusal below tells the agent something different: stop and
// re-register, stop and re-claim, or this grant is no longer yours.

func TestIntegrationTheHeartbeatsAnAttemptWillNotTake(t *testing.T) {
	st, pool, cert, cat, _, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	sound := protocol.Heartbeat{SchemaVersion: protocol.Version,
		AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
		AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
		Phase: "executing", HostFacts: facts}

	t.Run("an attempt that has not been acknowledged yet", func(t *testing.T) {
		// The attempt is still issued: the agent has not yet proved it
		// verified the source it was sent. Reporting progress on work
		// that was never allowed to start is a conflict, not an
		// invalid request, because the grant itself is sound and the
		// agent's own next step is to acknowledge.
		var state string
		if err := pool.QueryRow(ctx, "SELECT state FROM task_attempts WHERE id=$1", grant.AttemptID).Scan(&state); err != nil {
			t.Fatal(err)
		}
		if state != "issued" {
			t.Fatalf("the fixture handed over an attempt already at %q", state)
		}
		if _, err := st.HeartbeatAgentAttempt(ctx, cert, sound); !errors.Is(err, ErrConflict) {
			t.Fatalf("an unacknowledged attempt accepted a heartbeat: %v", err)
		}
	})

	t.Run("a host that is not the one the agent registered", func(t *testing.T) {
		// Judged ahead of the attempt's own state, so a heartbeat from
		// the wrong host is denied whether or not the work is running.
		// The facts stay internally consistent (windows reports its
		// own load kind) so this is the plane comparing against the
		// registered agent, not the schema refusing a bad shape.
		moved := sound
		moved.HostFacts.OS = "windows"
		moved.HostFacts.LoadKind = "cpu_busy_equivalent"
		if _, err := st.HeartbeatAgentAttempt(ctx, cert, moved); !errors.Is(err, ErrDenied) {
			t.Fatalf("a heartbeat from a different host was accepted: %v", err)
		}
	})

	t.Run("an assignment this plane never issued", func(t *testing.T) {
		unknown := sound
		unknown.AssignmentID = mustID(t)
		unknown.AttemptID = mustID(t)
		if _, err := st.HeartbeatAgentAttempt(ctx, cert, unknown); !errors.Is(err, ErrNotFound) {
			t.Fatalf("a heartbeat on an unknown assignment was not reported missing: %v", err)
		}
	})

	t.Run("a grant another agent has already overtaken", func(t *testing.T) {
		// Version and fence are checked together, and either one being
		// wrong means this agent is holding a grant that has moved.
		for name, spoil := range map[string]func(*protocol.Heartbeat){
			"a fencing token that has been superseded": func(h *protocol.Heartbeat) { h.FencingToken++ },
			"an assignment version that has moved":     func(h *protocol.Heartbeat) { h.AssignmentVersion++ },
		} {
			stale := sound
			spoil(&stale)
			if _, err := st.HeartbeatAgentAttempt(ctx, cert, stale); !errors.Is(err, ErrConflict) {
				t.Fatalf("accepted a heartbeat carrying %s: %v", name, err)
			}
		}
	})

	t.Run("an attempt that has already ended", func(t *testing.T) {
		// Acknowledge so the attempt runs, then end it underneath the
		// agent. A heartbeat arriving after the attempt is terminal is
		// refused rather than quietly stamping a finished row, which
		// is what keeps the reaper's view of liveness honest.
		ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
			AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
			FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
			SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
		if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
			t.Fatal(err)
		}
		if _, err := st.HeartbeatAgentAttempt(ctx, cert, sound); err != nil {
			t.Fatalf("a running attempt refused a sound heartbeat: %v", err)
		}
		if _, err := pool.Exec(ctx, `UPDATE task_attempts SET state='cancelled',
			ended_at=clock_timestamp(), version=version+1 WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
		if _, err := st.HeartbeatAgentAttempt(ctx, cert, sound); !errors.Is(err, ErrConflict) {
			t.Fatalf("an attempt that had ended accepted a heartbeat: %v", err)
		}
	})
}
