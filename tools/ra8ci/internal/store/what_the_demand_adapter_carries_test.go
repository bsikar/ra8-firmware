// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

// The demand adapter's other half. Its nil guards are pinned elsewhere; what
// is left is the pass-through, and it is worth pinning for one reason: both
// methods reach the store before the store reaches Postgres, so a refusal the
// store can make on the arguments alone must arrive at the caller as the
// refusal it was, not as an adapter error and not as a connection failure the
// caller would retry.
//
// A Store with no pool is the whole fixture. Neither store method consults the
// pool until its own argument check has passed, so the adapter's wiring is
// exercised with nothing running behind it.

func TestTheAdapterIsAViewOnWhicheverStoreMadeIt(t *testing.T) {
	plane := &Store{}
	source := plane.DemandSource()
	if source == nil || source.store != plane {
		t.Fatalf("the adapter does not view the store that made it: %#v", source)
	}
	// Two adapters off the same store are independent values over the same
	// plane, which is what makes one per call site safe.
	if other := plane.DemandSource(); other == source || other.store != plane {
		t.Fatalf("a second adapter is not a separate view: %#v", other)
	}
}

// A page size the store will not serve is refused by the store, and the
// adapter has to carry that verdict out unchanged. Widening it to a generic
// failure would turn a caller's own bad argument into something that reads
// like the database is down.
func TestAPageTheStoreWillNotServeIsRefusedThroughTheAdapter(t *testing.T) {
	source := (&Store{}).DemandSource()

	for _, limit := range []int{0, -1, 1001} {
		_, err := source.ListOpen(context.Background(), limit)
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("page size %d: err %v, want ErrInvalid", limit, err)
		}
	}
}

// The same for a write. demand.Event.Validate is what decides whether an
// event is writable at all, and it runs before any connection is asked for,
// so an unusable event must come back invalid rather than unavailable: one is
// the caller's to fix and the other is something they would sit and retry.
func TestAnEventTheStoreWillNotWriteIsRefusedThroughTheAdapter(t *testing.T) {
	source := (&Store{}).DemandSource()

	if err := source.Record(context.Background(), demand.Event{}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an empty event: err %v, want ErrInvalid", err)
	}
	if errors.Is(source.Record(context.Background(), demand.Event{}), ErrUnavailable) {
		t.Fatal("an unusable event was reported as the plane being unavailable")
	}

	// An event missing one required field is refused the same way, so the
	// refusal above is the validation and not the empty value.
	partial := sourceDemandFixture(41, 1, demand.PhaseQueued)
	partial.Repository = ""
	if err := source.Record(context.Background(), partial); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an event with no repository: err %v, want ErrInvalid", err)
	}
}
