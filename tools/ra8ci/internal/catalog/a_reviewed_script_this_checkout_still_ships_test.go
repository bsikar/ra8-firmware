// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package catalog

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

// repositoryRoot is this package's own directory walked back to the checkout
// it ships in: tools/ra8ci/internal/catalog.
func repositoryRoot(t *testing.T) string {
	t.Helper()
	working, err := os.Getwd()
	if err != nil {
		t.Fatalf("working directory: %v", err)
	}
	root := filepath.Join(working, "..", "..", "..", "..")
	if _, err := os.Stat(filepath.Join(root, ".git")); err != nil {
		t.Skipf("not running inside a checkout: %v", err)
	}
	return root
}

// The reviewed script list is restated rather than read off disk, and that is
// the right call at review time: the checkout a manifest is admitted against
// is not the checkout it runs in. It leaves one gap the review cannot close by
// itself, which is the one a_script_the_checkout_ships.go describes: a script
// that was renamed still looks right in the list, ships under a reviewed
// digest, and surfaces on a runner as bash answering exit 127 having run
// nothing, once per dispatch, forever.
//
// A test can close it, because a test does run in the checkout. This is the
// rename detector the list cannot be.
func TestEveryReviewedScriptExistsInThisCheckout(t *testing.T) {
	root := repositoryRoot(t)
	paths := ReviewedScriptPaths()
	if len(paths) == 0 {
		t.Fatal("no scripts are reviewed for dispatch")
	}
	for _, script := range paths {
		info, err := os.Stat(filepath.Join(root, script))
		if err != nil {
			t.Fatalf("reviewed script %q is not in this checkout: %v", script, err)
		}
		if !info.Mode().IsRegular() {
			t.Fatalf("reviewed script %q is not a regular file", script)
		}
		if runtime.GOOS != "windows" && info.Mode().Perm()&0111 == 0 {
			t.Fatalf("reviewed script %q is not executable, so bash dispatch would fail on a runner", script)
		}
		if info.Size() == 0 {
			t.Fatalf("reviewed script %q is empty", script)
		}
	}
}

// The same gap on the other side of the pair: a script the shipped catalog
// dispatches that nobody reviewed. TestTheShippedCatalogDispatchesOnlyReviewedScripts
// holds the list to the catalog; this holds the catalog's scripts to the
// checkout, so a rename cannot pass by editing only one of the three places.
func TestEveryScriptTheShippedCatalogDispatchesExists(t *testing.T) {
	root := repositoryRoot(t)
	definitions, err := Load()
	if err != nil {
		t.Fatalf("load catalog: %v", err)
	}
	seen := map[string]bool{}
	for _, name := range definitions.Names() {
		task, found := definitions.Task(name)
		if !found {
			t.Fatalf("task %q vanished between listing and lookup", name)
		}
		for _, step := range task.Steps {
			if step.Program != DispatchShell || len(step.Args) == 0 {
				continue
			}
			script := step.Args[0]
			if seen[script] {
				continue
			}
			seen[script] = true
			if _, err := os.Stat(filepath.Join(root, script)); err != nil {
				t.Fatalf("task %q step %q dispatches %q, which is not in this checkout: %v",
					name, step.Name, script, err)
			}
		}
	}
	if len(seen) == 0 {
		t.Fatal("the shipped catalog dispatches no script at all")
	}
}
