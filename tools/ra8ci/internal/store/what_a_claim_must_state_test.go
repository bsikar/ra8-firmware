// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"
)

// What a claim must state before the plane will open a transaction for it.
//
// StartAttempt is the local claim path, and the host facts it carries are
// what a later reader uses to explain a result: which engine ran the work,
// on a machine of what size, under what load. A claim that cannot describe
// its own host is refused before any task is read, so the attempt row never
// records a host nobody can reason about.
//
// The first refusal is a different kind. A remote agent must come through
// the fenced claim path, and sending it here instead is not a malformed
// request but a request on the wrong door: the fencing token that keeps two
// agents off one task only exists on the other path.

func TestAClaimFromARemoteAgentIsSentToTheFencedPath(t *testing.T) {
	good, err := NewID()
	if err != nil {
		t.Fatalf("minting an identifier: %v", err)
	}

	// Everything else about this claim is impeccable. It is refused for
	// carrying an agent at all, and the refusal is checked ahead of the
	// host facts, so a remote caller is told which door to use rather
	// than being sent away to fix a host description that is already fine.
	err = func() error {
		_, err := (&Store{}).StartAttempt(context.Background(), StartAttemptInput{
			TaskID: good, ActorID: "runner", AgentID: good, Engine: "local-command",
			HostCores: 4, HostRAMBytes: 1 << 30, HostLoad: 0.5,
		})
		return err
	}()
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a remote agent claimed through the local path: %v", err)
	}
	if !strings.Contains(err.Error(), "fenced ClaimAgentTask") {
		t.Fatalf("the refusal did not name the path to use: %v", err)
	}
}

func TestAClaimIsRefusedWhenItCannotDescribeItsHost(t *testing.T) {
	good, err := NewID()
	if err != nil {
		t.Fatalf("minting an identifier: %v", err)
	}
	sound := func() StartAttemptInput {
		return StartAttemptInput{
			TaskID: good, ActorID: "runner", Engine: "local-command",
			HostCores: 4, HostRAMBytes: 1 << 30, HostLoad: 0.5,
			HostFacts: json.RawMessage(`{"kernel":"6.1"}`),
		}
	}

	for _, refusal := range []struct {
		name  string
		spoil func(*StartAttemptInput)
	}{
		{"a malformed task identifier", func(in *StartAttemptInput) { in.TaskID = "task-3" }},
		{"no actor to claim on behalf of", func(in *StartAttemptInput) { in.ActorID = "" }},
		{"no engine named", func(in *StartAttemptInput) { in.Engine = "" }},
		{"an engine name past the bound", func(in *StartAttemptInput) { in.Engine = strings.Repeat("e", 65) }},
		{"a host with no cores", func(in *StartAttemptInput) { in.HostCores = 0 }},
		{"a host with fewer than no cores", func(in *StartAttemptInput) { in.HostCores = -1 }},
		{"a host with no memory", func(in *StartAttemptInput) { in.HostRAMBytes = 0 }},
		{"a negative load", func(in *StartAttemptInput) { in.HostLoad = -0.1 }},
		{"a load that is not a number", func(in *StartAttemptInput) { in.HostLoad = math.NaN() }},
		{"an unbounded load", func(in *StartAttemptInput) { in.HostLoad = math.Inf(1) }},
		{"host facts that are a list", func(in *StartAttemptInput) { in.HostFacts = json.RawMessage(`[]`) }},
		{"host facts that are a bare string", func(in *StartAttemptInput) { in.HostFacts = json.RawMessage(`"linux"`) }},
		{"host facts that are not JSON at all", func(in *StartAttemptInput) { in.HostFacts = json.RawMessage(`{`) }},
	} {
		t.Run(refusal.name, func(t *testing.T) {
			in := sound()
			refusal.spoil(&in)
			// A store with no database: reaching Postgres would
			// panic, so an answer at all proves the refusal landed
			// before the transaction.
			_, err := (&Store{}).StartAttempt(context.Background(), in)
			if !errors.Is(err, ErrInvalid) {
				t.Fatalf("accepted %s: %v", refusal.name, err)
			}
		})
	}

	// Absent host facts are not a refusal. A runner that knows nothing
	// worth recording about its host still gets to claim work; the
	// validator supplies an empty object rather than rejecting the claim,
	// and that is the line between "said nothing" and "said nonsense".
	silent := sound()
	silent.HostFacts = nil
	if !validHostFacts(silent) {
		t.Fatal("a claim with no host facts to add was refused")
	}
}
