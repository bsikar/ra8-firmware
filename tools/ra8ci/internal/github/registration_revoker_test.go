// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/demand"
)

type fakeRegistry struct {
	byName    map[string]RunnerIdentity
	byID      map[int]RunnerIdentity
	nameErr   error
	idErr     error
	removeErr error
	removed   []int
}

func (f *fakeRegistry) RunnerByName(_ context.Context, name string) (RunnerIdentity, bool, error) {
	if f.nameErr != nil {
		return RunnerIdentity{}, false, f.nameErr
	}
	runner, exists := f.byName[name]
	return runner, exists, nil
}

func (f *fakeRegistry) RunnerByID(_ context.Context, id int) (RunnerIdentity, bool, error) {
	if f.idErr != nil {
		return RunnerIdentity{}, false, f.idErr
	}
	runner, exists := f.byID[id]
	return runner, exists, nil
}

func (f *fakeRegistry) RemoveRunner(_ context.Context, id int) error {
	if f.removeErr != nil {
		return f.removeErr
	}
	f.removed = append(f.removed, id)
	return nil
}

func testRevoker(t *testing.T, registry *fakeRegistry) *RegistrationRevoker {
	t.Helper()
	revoker, err := NewRegistrationRevoker(registry)
	if err != nil {
		t.Fatalf("new revoker: %v", err)
	}
	return revoker
}

func TestNewRegistrationRevokerRefusesNoRegistry(t *testing.T) {
	if _, err := NewRegistrationRevoker(nil); err == nil {
		t.Fatal("a revoker with no registry was accepted")
	}
}

func TestRevokeRemovesTheRunnerTheReservationHolds(t *testing.T) {
	runner := RunnerIdentity{ID: 41, Name: "ra8ci-9001-1"}
	registry := &fakeRegistry{
		byName: map[string]RunnerIdentity{runner.Name: runner},
		byID:   map[int]RunnerIdentity{runner.ID: runner},
	}
	revoker := testRevoker(t, registry)
	revoked, err := revoker.Revoke(t.Context(), RunnerRef{ID: runner.ID, Name: runner.Name})
	if err != nil || !revoked {
		t.Fatalf("revoke: revoked=%v err=%v", revoked, err)
	}
	if len(registry.removed) != 1 || registry.removed[0] != runner.ID {
		t.Fatalf("removed %v, want [%d]", registry.removed, runner.ID)
	}
}

func TestRevokeIsIdempotentOnceTheRegistrationIsGone(t *testing.T) {
	// The resumed pass: the reaper walks the sequence from the top, so
	// the second time through there is nothing left on the forge.
	registry := &fakeRegistry{}
	revoker := testRevoker(t, registry)
	revoked, err := revoker.Revoke(t.Context(), RunnerRef{ID: 41, Name: "ra8ci-9001-1"})
	if err != nil || revoked {
		t.Fatalf("already-gone registration: revoked=%v err=%v", revoked, err)
	}
	if len(registry.removed) != 0 {
		t.Fatalf("removed something that was not there: %v", registry.removed)
	}
}

func TestRevokeOfAReservationThatNeverRegistered(t *testing.T) {
	// Demand cancelled between the reservation and the mint. Not an
	// error, and not a lookup either: there is nothing to ask about.
	registry := &fakeRegistry{nameErr: errors.New("the forge must not be called"), idErr: errors.New("the forge must not be called")}
	revoker := testRevoker(t, registry)
	revoked, err := revoker.Revoke(t.Context(), RunnerRef{})
	if err != nil || revoked {
		t.Fatalf("unregistered reservation: revoked=%v err=%v", revoked, err)
	}
}

func TestRevokeRefusesARunnerThisPlaneDidNotMint(t *testing.T) {
	foreign := RunnerIdentity{ID: 7, Name: "shared-builder-3"}
	registry := &fakeRegistry{
		byName: map[string]RunnerIdentity{foreign.Name: foreign},
		byID:   map[int]RunnerIdentity{foreign.ID: foreign},
	}
	revoker := testRevoker(t, registry)
	// Named on the reservation: refused before the forge is asked.
	if _, err := revoker.Revoke(t.Context(), RunnerRef{ID: foreign.ID, Name: foreign.Name}); !errors.Is(err, ErrForeignRunner) {
		t.Fatalf("foreign name: %v", err)
	}
	// Reached by id alone, and the forge answers with a name we never
	// minted. Same refusal, one step later.
	if _, err := revoker.Revoke(t.Context(), RunnerRef{ID: foreign.ID}); !errors.Is(err, ErrForeignRunner) {
		t.Fatalf("foreign id: %v", err)
	}
	if len(registry.removed) != 0 {
		t.Fatalf("removed a foreign runner: %v", registry.removed)
	}
}

func TestRevokeRefusesWhenTheForgeDisagreesWithTheLedger(t *testing.T) {
	registry := &fakeRegistry{
		byName: map[string]RunnerIdentity{"ra8ci-9001-1": {ID: 99, Name: "ra8ci-9001-1"}},
	}
	revoker := testRevoker(t, registry)
	_, err := revoker.Revoke(t.Context(), RunnerRef{ID: 41, Name: "ra8ci-9001-1"})
	if !errors.Is(err, ErrForeignRunner) {
		t.Fatalf("id disagreement: %v", err)
	}
	if len(registry.removed) != 0 {
		t.Fatalf("removed on a disagreement: %v", registry.removed)
	}
}

func TestRevokeByIDAloneUsesTheIDLookup(t *testing.T) {
	runner := RunnerIdentity{ID: 41, Name: "ra8ci-9001-2"}
	registry := &fakeRegistry{
		byID:    map[int]RunnerIdentity{runner.ID: runner},
		nameErr: errors.New("name lookup must not be used without a name"),
	}
	revoker := testRevoker(t, registry)
	revoked, err := revoker.Revoke(t.Context(), RunnerRef{ID: runner.ID})
	if err != nil || !revoked {
		t.Fatalf("revoke by id: revoked=%v err=%v", revoked, err)
	}
	if len(registry.removed) != 1 || registry.removed[0] != runner.ID {
		t.Fatalf("removed %v", registry.removed)
	}
}

func TestRevokeReportsLookupAndRemovalFailures(t *testing.T) {
	// A failing forge must not read as "nothing was registered": the
	// sequence stops here and the reservation stays in the queue.
	lookup := &fakeRegistry{nameErr: errors.New("forge unreachable")}
	if _, err := testRevoker(t, lookup).Revoke(t.Context(), RunnerRef{Name: "ra8ci-9001-1"}); err == nil {
		t.Fatal("a failed lookup passed as a revocation")
	}
	runner := RunnerIdentity{ID: 41, Name: "ra8ci-9001-1"}
	removal := &fakeRegistry{
		byName:    map[string]RunnerIdentity{runner.Name: runner},
		removeErr: errors.New("forge refused"),
	}
	revoked, err := testRevoker(t, removal).Revoke(t.Context(), RunnerRef{ID: runner.ID, Name: runner.Name})
	if err == nil || revoked {
		t.Fatalf("a failed removal passed: revoked=%v err=%v", revoked, err)
	}
}

func TestMintedAndRegisteredPredicates(t *testing.T) {
	for name, want := range map[string]bool{
		"ra8ci-9001-1":     true,
		"ra8ci-1":          true,
		"shared-builder-3": false,
		"":                 false,
		"ra8ci-9001 1":     false,
		"-ra8ci-9001-1":    false,
	} {
		if got := Minted(name); got != want {
			t.Fatalf("Minted(%q)=%v want %v", name, got, want)
		}
	}
	for ref, want := range map[RunnerRef]bool{
		{}:                       false,
		{ID: 1}:                  true,
		{Name: "ra8ci-9001-1"}:   true,
		{ID: 1, Name: "ra8ci-1"}: true,
	} {
		if got := ref.Registered(); got != want {
			t.Fatalf("%+v.Registered()=%v want %v", ref, got, want)
		}
	}
}

// A name minted for one unit of demand is exactly what the revoker will
// accept back, so registration and revocation cannot drift apart.
func TestMintedAcceptsEveryDemandRunnerName(t *testing.T) {
	for _, identity := range []struct {
		jobID   int64
		attempt int
	}{{1, 1}, {9223372036854775807, 1000}, {42, 7}} {
		name, err := DemandRunnerName(demand.Event{JobID: identity.jobID, RunAttempt: identity.attempt})
		if err != nil {
			t.Fatalf("name for %d/%d: %v", identity.jobID, identity.attempt, err)
		}
		if !Minted(name) {
			t.Fatalf("the revoker refuses a name registration minted: %q", name)
		}
	}
}
