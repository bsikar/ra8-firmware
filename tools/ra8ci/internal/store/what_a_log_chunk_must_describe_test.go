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

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// What a log chunk has to state before the plane will look at it.
//
// Log upload is the one call an agent makes continuously while work runs,
// so its cheapest refusal matters: every shape below is turned away ahead
// of the transaction, without a database and without the certificate being
// read. The digest rule is the one worth reading twice, because it is what
// stops an agent filing bytes under a hash that does not describe them.

func TestALogChunkMustDescribeItsOwnBytes(t *testing.T) {
	ctx := context.Background()
	assignmentID, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	attemptID, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	data := []byte("the agent said something\n")
	digest := sha256.Sum256(data)
	sound := protocol.LogChunk{SchemaVersion: protocol.Version,
		AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1, Sequence: 1,
		Stream: "stdout", StepName: "format-tree-check",
		DataBase64: base64.StdEncoding.EncodeToString(data),
		SHA256:     hex.EncodeToString(digest[:])}

	other := sha256.Sum256([]byte("the agent said something else\n"))
	for name, spoil := range map[string]func(*protocol.LogChunk){
		"no schema version":                       func(c *protocol.LogChunk) { c.SchemaVersion = 0 },
		"a schema version from a newer agent":     func(c *protocol.LogChunk) { c.SchemaVersion = protocol.Version + 1 },
		"no assignment":                           func(c *protocol.LogChunk) { c.AssignmentID = "" },
		"an assignment that is not an identifier": func(c *protocol.LogChunk) { c.AssignmentID = "not-an-id" },
		"no attempt":                              func(c *protocol.LogChunk) { c.AttemptID = "" },
		"no assignment version":                   func(c *protocol.LogChunk) { c.AssignmentVersion = 0 },
		"no fencing token":                        func(c *protocol.LogChunk) { c.FencingToken = 0 },
		"a sequence below one":                    func(c *protocol.LogChunk) { c.Sequence = 0 },
		"a negative sequence":                     func(c *protocol.LogChunk) { c.Sequence = -1 },
		"no stream":                               func(c *protocol.LogChunk) { c.Stream = "" },
		"a stream that is neither of the two":     func(c *protocol.LogChunk) { c.Stream = "stdlog" },
		"a stream in the wrong case":              func(c *protocol.LogChunk) { c.Stream = "Stdout" },
		"no step name":                            func(c *protocol.LogChunk) { c.StepName = "" },
		"no digest":                               func(c *protocol.LogChunk) { c.SHA256 = "" },
		"a digest that is not a digest":           func(c *protocol.LogChunk) { c.SHA256 = "not-a-sha" },
		"no bytes at all":                         func(c *protocol.LogChunk) { c.DataBase64 = "" },
		"bytes that are not base64":               func(c *protocol.LogChunk) { c.DataBase64 = "!!!!" },
		"a digest of different bytes":             func(c *protocol.LogChunk) { c.SHA256 = hex.EncodeToString(other[:]) },
		"bytes the digest does not describe": func(c *protocol.LogChunk) {
			c.DataBase64 = base64.StdEncoding.EncodeToString([]byte("something the agent never said\n"))
		},
	} {
		t.Run(name, func(t *testing.T) {
			// Only the named field is spoiled, so the refusal can only
			// be about this one thing.
			refused := sound
			spoil(&refused)
			// A zero-value store has no pool, so reaching one would
			// panic rather than return. Coming back with ErrInvalid is
			// the proof the shape was judged first.
			if err := (&Store{}).SaveAgentLog(ctx, []byte("any certificate"), refused); !errors.Is(err, ErrInvalid) {
				t.Fatalf("accepted a chunk with %s: %v", name, err)
			}
		})
	}

	t.Run("bytes beyond what one chunk may carry", func(t *testing.T) {
		// The ceiling is on the decoded bytes, not on the encoded
		// string, so the digest has to describe the oversized payload
		// for this to be the size refusal and not the digest one.
		oversized := []byte(strings.Repeat("x", protocol.MaxLogBytes+1))
		sum := sha256.Sum256(oversized)
		huge := sound
		huge.DataBase64 = base64.StdEncoding.EncodeToString(oversized)
		huge.SHA256 = hex.EncodeToString(sum[:])
		if err := (&Store{}).SaveAgentLog(ctx, []byte("any certificate"), huge); !errors.Is(err, ErrInvalid) {
			t.Fatalf("accepted a chunk carrying more than one chunk may: %v", err)
		}
	})

	t.Run("a sound chunk is not refused on its shape", func(t *testing.T) {
		// The cases above only mean something if the sound chunk gets
		// past the same guard. It has to reach a pool that is not
		// there, so whatever comes back must not be the shape refusal.
		defer func() { _ = recover() }()
		if err := (&Store{}).SaveAgentLog(ctx, []byte("any certificate"), sound); errors.Is(err, ErrInvalid) {
			t.Fatalf("a sound chunk was turned away on its shape: %v", err)
		}
	})
}
