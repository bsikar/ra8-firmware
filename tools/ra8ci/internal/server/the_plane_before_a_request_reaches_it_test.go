// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package server

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Three seams decide what a plane is before any request reaches it: the floor
// its constructor holds readiness checks to, the answer those checks give when
// they all pass, and which half of the plane a denial is recorded against.
// None of the three needs a database, and none of them had been the answer to
// anything: the constructor's readiness floor was never tripped, the readiness
// walk was only ever driven down its failing arm, and the audit fallback was
// only ever read on a plane that already had an auditor installed.

// TestAPlaneRefusesAReadinessCheckItCannotCall pins the constructor's floor.
// A nil check in the list is not an empty one: it would be called on every
// readiness probe and panic there, turning a health endpoint into the thing
// that takes the plane down. It is refused at construction, where an operator
// is still watching, rather than at the first probe.
func TestAPlaneRefusesAReadinessCheckItCannotCall(t *testing.T) {
	cat := reviewedCatalog(t)

	if _, err := NewWithOptions(&store.Store{}, cat, nil, "", nil); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a plane carrying a readiness check it cannot call was built: %v", err)
	}

	// The refusal reads the whole list, not just its head: a sound check
	// ahead of a nil one must not vouch for it.
	sound := func(context.Context) error { return nil }
	if _, err := NewWithOptions(&store.Store{}, cat, nil, "", sound, nil); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("a nil readiness check behind a sound one was accepted: %v", err)
	}

	api, err := NewWithOptions(&store.Store{}, cat, nil, "", sound, sound)
	if err != nil {
		t.Fatalf("a plane whose readiness checks are all callable was refused: %v", err)
	}
	if api == nil {
		t.Fatal("a plane was built as nil without an error")
	}
}

// TestReadinessAnswersWhenEveryCheckDoes pins the walk's other end. The
// failing arm says which dependency is out; this one is what lets the plane
// say it is ready at all, and a plane configured with no checks is ready by
// that same walk rather than by a separate rule.
func TestReadinessAnswersWhenEveryCheckDoes(t *testing.T) {
	called := 0
	sound := func(context.Context) error { called++; return nil }

	api := &Server{readinessChecks: []func(context.Context) error{sound, sound, sound}}
	if err := api.checkReadiness(context.Background()); err != nil {
		t.Fatalf("a plane whose dependencies all answered was held unready: %v", err)
	}
	if called != 3 {
		t.Fatalf("%d of 3 readiness checks were called; a skipped check is a dependency nobody asked about", called)
	}

	bare := &Server{}
	if err := bare.checkReadiness(context.Background()); err != nil {
		t.Fatalf("a plane configured with no readiness checks was held unready: %v", err)
	}
}

// TestADenialIsRecordedAgainstTheStoreWhenNothingElseIsInstalled pins the
// audit fallback. What a denial writes is a security property: it is the only
// record that a certificate asked for something it was not granted. The
// installed auditor is a test seam, so on a real plane the store itself is the
// auditor, and that fallback is what keeps a refused request from passing
// unrecorded.
func TestADenialIsRecordedAgainstTheStoreWhenNothingElseIsInstalled(t *testing.T) {
	backing := &store.Store{}

	plane := &Server{store: backing}
	auditor := plane.denialAudit()
	if auditor == nil {
		t.Fatal("a plane with a store had no auditor, so a denial would go unrecorded")
	}
	if auditor != denialAuditor(backing) {
		t.Fatal("a denial would be recorded somewhere other than the plane's own store")
	}

	// An installed auditor still wins: that is what lets a test observe what
	// a denial writes without a database behind it.
	watcher := &countingDenialAuditor{}
	watched := &Server{store: backing, audit: watcher}
	if watched.denialAudit() != denialAuditor(watcher) {
		t.Fatal("an installed auditor was passed over in favour of the store")
	}

	// A plane with neither says so rather than handing back something that
	// cannot be called.
	if (&Server{}).denialAudit() != nil {
		t.Fatal("a plane with nothing to audit against named an auditor")
	}
}

type countingDenialAuditor struct{ seen int }

func (c *countingDenialAuditor) AuditDenied(context.Context, string, string, string) error {
	c.seen++
	return nil
}
