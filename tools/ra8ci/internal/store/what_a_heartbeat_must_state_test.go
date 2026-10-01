// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What a heartbeat has to state before the plane will look at it.
//
// The schema is judged ahead of the transaction, so every shape below is
// refused without a database and without the certificate being read. That
// ordering is the point: a malformed heartbeat costs the plane nothing, and
// an agent sending one learns its own request was wrong rather than being
// told the assignment is missing or stale.

func TestAHeartbeatMustStateItsGrantAndPhase(t *testing.T) {
	ctx := context.Background()
	facts := protocol.HostFacts{Cores: 4, RAMBytes: 8 << 30, RAMFreeBytes: 4 << 30,
		Load1: 0.5, LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: time.Now().UTC()}
	assignmentID, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	attemptID, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	sound := protocol.Heartbeat{SchemaVersion: protocol.Version,
		AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1, Phase: "executing", HostFacts: facts}

	for name, spoil := range map[string]func(*protocol.Heartbeat){
		"no schema version":                        func(h *protocol.Heartbeat) { h.SchemaVersion = 0 },
		"a schema version from a newer agent":      func(h *protocol.Heartbeat) { h.SchemaVersion = protocol.Version + 1 },
		"no assignment":                            func(h *protocol.Heartbeat) { h.AssignmentID = "" },
		"an assignment that is not an identifier":  func(h *protocol.Heartbeat) { h.AssignmentID = "not-an-id" },
		"no attempt":                               func(h *protocol.Heartbeat) { h.AttemptID = "" },
		"an attempt that is not an identifier":     func(h *protocol.Heartbeat) { h.AttemptID = "not-an-id" },
		"no assignment version":                    func(h *protocol.Heartbeat) { h.AssignmentVersion = 0 },
		"a negative assignment version":            func(h *protocol.Heartbeat) { h.AssignmentVersion = -1 },
		"no fencing token":                         func(h *protocol.Heartbeat) { h.FencingToken = 0 },
		"a negative fencing token":                 func(h *protocol.Heartbeat) { h.FencingToken = -1 },
		"no phase":                                 func(h *protocol.Heartbeat) { h.Phase = "" },
		"a phase that is not a live one":           func(h *protocol.Heartbeat) { h.Phase = "succeeded" },
		"a phase in the wrong case":                func(h *protocol.Heartbeat) { h.Phase = "Executing" },
		"host facts measured nowhere":              func(h *protocol.Heartbeat) { h.HostFacts.CapturedAt = time.Time{} },
		"an operating system nobody runs":          func(h *protocol.Heartbeat) { h.HostFacts.OS = "plan9" },
		"a load kind that disagrees with the host": func(h *protocol.Heartbeat) { h.HostFacts.LoadKind = "cpu_busy_equivalent" },
	} {
		t.Run(name, func(t *testing.T) {
			// Only the named field is spoiled: everything else stays
			// sound, so the refusal can only be about this one thing.
			refused := sound
			spoil(&refused)
			// A zero-value store has no pool at all, so reaching one
			// would panic rather than return. Arriving back with
			// ErrInvalid is the proof the schema was judged first.
			if _, err := (&Store{}).HeartbeatAgentAttempt(ctx, []byte("any certificate"), refused); !errors.Is(err, ErrInvalid) {
				t.Fatalf("accepted a heartbeat with %s: %v", name, err)
			}
		})
	}

	t.Run("a sound heartbeat is not refused on its shape", func(t *testing.T) {
		// The guard above only means something if the sound heartbeat
		// gets past it. This one therefore has to reach the pool a
		// zero-value store does not have, so whatever comes back, it
		// must not be the schema refusal: that is the evidence the
		// sixteen cases above were each refused for their own reason
		// and not because the baseline was malformed all along.
		defer func() {
			// Reaching a pool that is not there is a fine outcome.
			_ = recover()
		}()
		if _, err := (&Store{}).HeartbeatAgentAttempt(ctx, []byte("any certificate"), sound); errors.Is(err, ErrInvalid) {
			t.Fatalf("a sound heartbeat was turned away on its shape: %v", err)
		}
	})
}
