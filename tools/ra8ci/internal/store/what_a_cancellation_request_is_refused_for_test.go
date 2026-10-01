// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// What a cancellation request is refused for before any run is read.
//
// Cancellation is the one operator action that reaches across a whole run,
// so the identity it is recorded under has to be good before the plane opens
// a transaction: cancel_requested_by is what an operator reads months later
// to find out who stopped the run. These refusals land before the pool is
// touched, which is what lets a store with no database answer them at all.

func TestACancellationRequestIsRefusedWithoutAnIdentityItCanRecord(t *testing.T) {
	// A real identifier for the cases that are about the actor, so each one
	// is refused for the reason it names and not for a malformed run.
	good, err := NewID()
	if err != nil {
		t.Fatalf("minting an identifier: %v", err)
	}
	for _, one := range []struct {
		name  string
		runID string
		actor string
	}{
		{"no run", "", "operator"},
		{"a run identifier that is not one", "not-an-id", "operator"},
		{"a run identifier of the right length but the wrong alphabet", strings.Repeat("g", 36), "operator"},
		{"no actor", good, ""},
		{"an actor too long to record", good, strings.Repeat("a", 257)},
	} {
		t.Run(one.name, func(t *testing.T) {
			if _, err := (&Store{}).RequestRunCancellation(context.Background(), one.runID, one.actor); !errors.Is(err, ErrInvalid) {
				t.Fatalf("cancellation accepted: %v", err)
			}
		})
	}
}
