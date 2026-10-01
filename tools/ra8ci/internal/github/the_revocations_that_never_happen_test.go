// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// A revocation that cannot be carried out must never read as one that was.
// The caller treats false, nil as "there is nothing left to remove" and
// closes the reservation out, so every refusal below has to arrive as an
// error rather than as a quiet false.

// An incomplete revoker is refused before anything is looked up. It arrives
// as a non-nil Revoker to whatever holds it, so the nil receiver is the one
// shape that could otherwise dereference its way to a panic.
func TestAnIncompleteRevokerRemovesNothing(t *testing.T) {
	registry := &fakeRegistry{byName: map[string]RunnerIdentity{
		"ra8ci-9001-1": {ID: 41, Name: "ra8ci-9001-1"},
	}}
	ref := RunnerRef{ID: 41, Name: "ra8ci-9001-1"}

	var missing *RegistrationRevoker
	if revoked, err := missing.Revoke(context.Background(), ref); err == nil || revoked {
		t.Fatalf("a revoker that was never built answered revoked=%v err=%v", revoked, err)
	}

	hollow := &RegistrationRevoker{}
	if revoked, err := hollow.Revoke(context.Background(), ref); err == nil || revoked {
		t.Fatalf("a revoker with no registry answered revoked=%v err=%v", revoked, err)
	}

	// A nil context is exactly what this refusal is for.
	var none context.Context
	revoked, err := testRevoker(t, registry).Revoke(none, ref)
	if err == nil || revoked {
		t.Fatalf("a revocation with no context answered revoked=%v err=%v", revoked, err)
	}
	if !strings.Contains(err.Error(), "invalid registration revocation") {
		t.Fatalf("the refusal was named something else: %v", err)
	}
	if len(registry.removed) != 0 {
		t.Fatalf("an incomplete revocation removed %v", registry.removed)
	}
}

// A forge that answers a name query with a differently-named runner is
// refused, and nothing is removed. The name is the half derived from demand
// identity, so a disagreement there means the ledger is describing a
// different runner than the one the forge just handed back.
func TestANameTheForgeDisagreesWithStopsTheRevocation(t *testing.T) {
	// The control: a reservation the forge agrees with is revoked.
	agreed := RunnerIdentity{ID: 41, Name: "ra8ci-9001-1"}
	registry := &fakeRegistry{byName: map[string]RunnerIdentity{agreed.Name: agreed}}
	revoked, err := testRevoker(t, registry).Revoke(context.Background(),
		RunnerRef{ID: 41, Name: agreed.Name})
	if err != nil || !revoked {
		t.Fatalf("an agreed reservation was not revoked: revoked=%v err=%v", revoked, err)
	}

	// The same query, answered with a runner carrying another minted name.
	// The id still matches; the name does not.
	disagreeing := &fakeRegistry{byName: map[string]RunnerIdentity{
		"ra8ci-9001-1": {ID: 41, Name: "ra8ci-9001-2"},
	}}
	revoked, err = testRevoker(t, disagreeing).Revoke(context.Background(),
		RunnerRef{ID: 41, Name: "ra8ci-9001-1"})
	if !errors.Is(err, ErrForeignRunner) || revoked {
		t.Fatalf("a disagreeing name answered revoked=%v err=%v", revoked, err)
	}
	if !strings.Contains(err.Error(), `reservation holds runner "ra8ci-9001-1"`) ||
		!strings.Contains(err.Error(), `the forge holds "ra8ci-9001-2"`) {
		t.Fatalf("the refusal named neither runner: %v", err)
	}
	if len(disagreeing.removed) != 0 {
		t.Fatalf("a disagreeing name still removed %v", disagreeing.removed)
	}

	// A reservation naming a runner this plane never minted is refused at
	// the lookup, before the forge is asked anything at all.
	unminted := &fakeRegistry{}
	revoked, err = testRevoker(t, unminted).Revoke(context.Background(),
		RunnerRef{Name: "someone-elses-runner"})
	if !errors.Is(err, ErrForeignRunner) || revoked {
		t.Fatalf("an unminted name answered revoked=%v err=%v", revoked, err)
	}
}

// A lookup by id that fails is reported as the lookup it was. The id path
// is the one a reservation takes when the name was never recorded, and a
// forge failing there must not be mistaken for a runner already gone.
func TestAFailedLookupByIDIsNotARunnerAlreadyGone(t *testing.T) {
	registry := &fakeRegistry{idErr: errors.New("forge unreachable")}
	revoked, err := testRevoker(t, registry).Revoke(context.Background(), RunnerRef{ID: 41})
	if err == nil || revoked {
		t.Fatalf("a failed id lookup answered revoked=%v err=%v", revoked, err)
	}
	if !strings.Contains(err.Error(), "look up runner 41") ||
		!strings.Contains(err.Error(), "forge unreachable") {
		t.Fatalf("the failure lost its subject or its reason: %v", err)
	}
	if len(registry.removed) != 0 {
		t.Fatalf("a failed lookup removed %v", registry.removed)
	}

	// An id the forge simply does not hold is the ordinary second pass of
	// a resumed revocation, and is not an error.
	gone := &fakeRegistry{byID: map[int]RunnerIdentity{}}
	revoked, err = testRevoker(t, gone).Revoke(context.Background(), RunnerRef{ID: 41})
	if err != nil || revoked {
		t.Fatalf("an already-gone runner answered revoked=%v err=%v", revoked, err)
	}
}
