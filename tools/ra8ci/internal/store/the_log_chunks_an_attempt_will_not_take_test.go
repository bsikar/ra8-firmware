//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// The log chunks an attempt will not take.
//
// Evidence is only worth keeping if it belongs to work that was actually
// running when it was written. These are the four states where a perfectly
// well formed chunk is still refused, which is what stops an agent filing
// output against an attempt it no longer holds.

func TestIntegrationTheLogChunksAnAttemptWillNotTake(t *testing.T) {
	st, pool, cert, cat, _, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	data := []byte("the agent said something\n")
	digest := sha256.Sum256(data)
	chunkAt := func(sequence int64) protocol.LogChunk {
		return protocol.LogChunk{SchemaVersion: protocol.Version,
			AssignmentID: grant.AssignmentID, AttemptID: grant.AttemptID,
			AssignmentVersion: grant.AssignmentVersion, FencingToken: grant.FencingToken,
			Sequence: sequence, Stream: "stdout", StepName: "format-tree-check",
			DataBase64: base64.StdEncoding.EncodeToString(data),
			SHA256:     hex.EncodeToString(digest[:])}
	}

	t.Run("an attempt that has not been acknowledged yet", func(t *testing.T) {
		// The attempt is issued but not running: the agent has not yet
		// proved it verified the source it was sent, so there is no
		// work for this output to be evidence of.
		if err := st.SaveAgentLog(ctx, cert, chunkAt(1)); !errors.Is(err, ErrConflict) {
			t.Fatalf("an unacknowledged attempt accepted log output: %v", err)
		}
	})

	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatal(err)
	}

	t.Run("a running attempt takes the same chunk", func(t *testing.T) {
		// The baseline every refusal below is measured against: the
		// only thing that changes afterwards is the attempt's state or
		// the agent's standing, never the chunk itself.
		if err := st.SaveAgentLog(ctx, cert, chunkAt(1)); err != nil {
			t.Fatalf("a running attempt refused sound log output: %v", err)
		}
	})

	t.Run("an assignment this plane never issued", func(t *testing.T) {
		unknown := chunkAt(2)
		unknown.AssignmentID = mustID(t)
		unknown.AttemptID = mustID(t)
		if err := st.SaveAgentLog(ctx, cert, unknown); !errors.Is(err, ErrNotFound) {
			t.Fatalf("log output on an unknown assignment was not reported missing: %v", err)
		}
	})

	t.Run("a grant another agent has already overtaken", func(t *testing.T) {
		for name, spoil := range map[string]func(*protocol.LogChunk){
			"a fencing token that has been superseded": func(c *protocol.LogChunk) { c.FencingToken++ },
			"an assignment version that has moved":     func(c *protocol.LogChunk) { c.AssignmentVersion++ },
		} {
			stale := chunkAt(2)
			spoil(&stale)
			if err := st.SaveAgentLog(ctx, cert, stale); !errors.Is(err, ErrConflict) {
				t.Fatalf("accepted log output carrying %s: %v", name, err)
			}
		}
	})

	t.Run("output arriving after the evidence grace has passed", func(t *testing.T) {
		// Past the deadline plus its grace, the reaper owns this
		// attempt, so late output must not land underneath it. The
		// deadline is moved rather than waited out, and put back
		// afterwards so the next case starts from a live attempt.
		if _, err := pool.Exec(ctx, `UPDATE task_attempts
			SET deadline_at=clock_timestamp()-interval '2 minutes' WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
		if err := st.SaveAgentLog(ctx, cert, chunkAt(2)); !errors.Is(err, ErrConflict) {
			t.Fatalf("log output past the evidence grace was accepted: %v", err)
		}
		if _, err := pool.Exec(ctx, `UPDATE task_attempts
			SET deadline_at=clock_timestamp()+interval '10 minutes' WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
		if err := st.SaveAgentLog(ctx, cert, chunkAt(2)); err != nil {
			t.Fatalf("a live attempt refused log output after the deadline was restored: %v", err)
		}
	})

	t.Run("an attempt that has already ended", func(t *testing.T) {
		if _, err := pool.Exec(ctx, `UPDATE task_attempts SET state='cancelled',
			ended_at=clock_timestamp(), version=version+1 WHERE id=$1`, grant.AttemptID); err != nil {
			t.Fatal(err)
		}
		if err := st.SaveAgentLog(ctx, cert, chunkAt(3)); !errors.Is(err, ErrConflict) {
			t.Fatalf("an attempt that had ended accepted log output: %v", err)
		}
		// Nothing was written for the refused sequence, so the next
		// agent to hold this attempt starts from a gap-free record.
		var stored int64
		if err := pool.QueryRow(ctx, "SELECT COALESCE(MAX(seq),0) FROM log_chunks WHERE attempt_id=$1", grant.AttemptID).Scan(&stored); err != nil {
			t.Fatal(err)
		}
		if stored != 2 {
			t.Fatalf("the refused chunk left the log at sequence %d", stored)
		}
	})
}
