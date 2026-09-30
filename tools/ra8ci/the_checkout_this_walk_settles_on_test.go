// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// findCheckout walks up from the working directory until something answers to
// .git, and hands whatever it finds to catalog.VerifyCheckout. Two properties
// decide what a local command ends up running, and neither is obvious from
// the loop:
//
// Where it STOPS. The first marker wins, and the walk never carries on past
// it looking for a better one. A developer standing in a submodule or a
// nested checkout gets that inner tree verified and refused on its own terms,
// rather than the outer repository silently standing in for it.
//
// What counts as a MARKER. The check is a stat, not a directory check, so the
// .git FILE that a submodule or a linked worktree carries answers exactly as a
// .git directory does. That is the right call, and it is one character away
// from not being true.

// noCheckoutAbove skips the test when the temporary tree really does sit
// inside a checkout, so the refusal below is about the walk rather than about
// where this box happens to put its temporary files.
func noCheckoutAbove(t *testing.T, directory string) {
	t.Helper()
	for {
		if _, err := os.Stat(filepath.Join(directory, ".git")); err == nil {
			t.Skipf("temporary tree sits inside a checkout at %s", directory)
		}
		parent := filepath.Dir(directory)
		if parent == directory {
			return
		}
		directory = parent
	}
}

// A directory with no marker anywhere above it is refused by name. The walk
// ends at the filesystem root rather than spinning on it.
func TestAWalkThatFindsNoMarkerSaysSoRatherThanGuessing(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	t.Chdir(directory)

	root, err := findCheckout()
	if err == nil {
		t.Fatalf("findCheckout settled on %q with no marker above it", root)
	}
	if !strings.Contains(err.Error(), "no repository checkout found") {
		t.Fatalf("error = %q; want the absent checkout named", err)
	}
}

// The first marker wins. The tree below is not a real checkout, so
// verification refuses it, and that refusal IS the assertion: the walk handed
// this directory over instead of climbing past it to somewhere that might
// have verified.
func TestTheWalkStopsAtTheFirstMarkerRatherThanTheBestOne(t *testing.T) {
	for _, marker := range []string{"directory", "file"} {
		t.Run("a .git "+marker, func(t *testing.T) {
			base := t.TempDir()
			noCheckoutAbove(t, base)
			inner := filepath.Join(base, "inner")
			deep := filepath.Join(inner, "one", "two")
			if err := os.MkdirAll(deep, 0o755); err != nil {
				t.Fatal(err)
			}
			switch marker {
			case "directory":
				if err := os.Mkdir(filepath.Join(inner, ".git"), 0o755); err != nil {
					t.Fatal(err)
				}
			case "file":
				// What a submodule or a linked worktree actually carries.
				if err := os.WriteFile(filepath.Join(inner, ".git"),
					[]byte("gitdir: ../.git/modules/inner\n"), 0o644); err != nil {
					t.Fatal(err)
				}
			}
			t.Chdir(deep)

			_, err := findCheckout()
			if err == nil {
				t.Fatal("a tree that is not a checkout was verified")
			}
			if strings.Contains(err.Error(), "no repository checkout found") {
				t.Fatalf("error = %q; the walk climbed past the marker it found", err)
			}
		})
	}
}

// And the marker has to be the name, not a prefix of it: a sibling called
// .gitignore is what nearly every checkout-adjacent directory holds, and it
// is not a checkout.
func TestANeighbourOfTheMarkerIsNotAMarker(t *testing.T) {
	directory := t.TempDir()
	noCheckoutAbove(t, directory)
	for _, name := range []string{".gitignore", ".gitattributes", "git"} {
		if err := os.WriteFile(filepath.Join(directory, name), []byte("x\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	t.Chdir(directory)

	root, err := findCheckout()
	if err == nil {
		t.Fatalf("findCheckout settled on %q beside a .gitignore", root)
	}
	if !strings.Contains(err.Error(), "no repository checkout found") {
		t.Fatalf("error = %q; want the absent checkout named", err)
	}
}
