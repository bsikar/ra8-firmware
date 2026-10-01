// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"os"
	"strings"
	"testing"
)

func beganWith(t *testing.T, tier, scope string) error {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	outbox, err := Open(directory)
	if err != nil {
		t.Fatal(err)
	}
	_, err = outbox.BeginWithMetadata("format-check", strings.Repeat("a", 64), Metadata{
		Source: SourceIdentity{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("b", 40),
			Verification: "unverified"}, Tier: tier, Scope: scope, DeadlineSeconds: 900,
	})
	return err
}

func TestEveryTierTheColumnHoldsIsBegun(t *testing.T) {
	for _, tier := range filableTiers {
		if err := checkTheClassificationIsOneThePlaneFiles(tier, "safe-local-read-only"); err != nil {
			t.Fatalf("tier %q was refused: %v", tier, err)
		}
	}
}

func TestEveryScopeTheColumnHoldsIsBegun(t *testing.T) {
	for _, scope := range filableScopes {
		if err := checkTheClassificationIsOneThePlaneFiles("required", scope); err != nil {
			t.Fatalf("scope %q was refused: %v", scope, err)
		}
	}
}

func TestATierNoColumnHoldsIsRefused(t *testing.T) {
	err := checkTheClassificationIsOneThePlaneFiles("urgent", "safe-local-read-only")
	if !errors.Is(err, errUnfilableClassification) {
		t.Fatalf("an unfilable tier was accepted: %v", err)
	}
}

func TestAScopeNoColumnHoldsIsRefused(t *testing.T) {
	err := checkTheClassificationIsOneThePlaneFiles("required", "safe-local-write-anywhere")
	if !errors.Is(err, errUnfilableClassification) {
		t.Fatalf("an unfilable scope was accepted: %v", err)
	}
}

func TestTheClassificationIsComparedExactly(t *testing.T) {
	// The column compares bytes, so case and padding are not near misses.
	for _, tier := range []string{"Required", "required ", " required"} {
		if err := checkTheClassificationIsOneThePlaneFiles(tier, "safe-local-read-only"); !errors.Is(err, errUnfilableClassification) {
			t.Fatalf("tier %q was accepted: %v", tier, err)
		}
	}
}

func TestBeginRefusesATierThePlaneWillNotFile(t *testing.T) {
	// The wiring test: the rule has to run from the public entry point,
	// before the first command of the run is spent.
	if err := beganWith(t, "urgent", "safe-local-read-only"); !errors.Is(err, errUnfilableClassification) {
		t.Fatalf("Begin took an unfilable tier: %v", err)
	}
}

func TestBeginRefusesAScopeThePlaneWillNotFile(t *testing.T) {
	if err := beganWith(t, "required", "safe-local-write-anywhere"); !errors.Is(err, errUnfilableClassification) {
		t.Fatalf("Begin took an unfilable scope: %v", err)
	}
}

func TestBeginTakesAnOrdinaryClassification(t *testing.T) {
	if err := beganWith(t, "nightly", "safe-local-write-working-tree"); err != nil {
		t.Fatalf("an ordinary classification was refused: %v", err)
	}
}
