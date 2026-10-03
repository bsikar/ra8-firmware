// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The version is read before any scope is derived, so a tree with a readable
// VERSION and no repository behind it reaches the sweep and is refused there.
// A scope the gate cannot derive is never a clean tree.
func TestAnAllSweepOutsideARepositoryIsRefused(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte("1.4.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	got := gate(t, root, "--all")
	if got.code != 2 {
		t.Fatalf("a sweep outside a repository answered %+v", got)
	}
	if !strings.Contains(got.stderr, "git ls-files failed") {
		t.Fatalf("the refusal did not name the derivation that failed: %q", got.stderr)
	}
	if got.stdout != "" {
		t.Fatalf("stdout carried %q", got.stdout)
	}
}

// A tracked name that is not a regular file when the sweep reaches it is
// passed over rather than refused. Both shapes get there through git, which
// lists a dangling symlink and a directory alike, and neither is source the
// gate can judge.
func TestATrackedNameThatIsNotAFileIsPassedOver(t *testing.T) {
	root := plantRepo(t, "1.4.0", firstParty)
	inc := filepath.Join(root, "libs", "ra8_thing", "inc")
	if err := os.MkdirAll(inc, 0o755); err != nil {
		t.Fatal(err)
	}
	symlinkTest(t, filepath.Join(inc, "nothing_here.h"), filepath.Join(inc, "gone.h"))
	if err := os.MkdirAll(filepath.Join(inc, "folder.h"), 0o755); err != nil {
		t.Fatal(err)
	}

	got := gate(t, root, "--all")
	if got.code != 0 {
		t.Fatalf("a sweep over a tree whose only oddities are unreadable names answered %+v", got)
	}
	for _, name := range []string{"gone.h", "folder.h"} {
		if strings.Contains(got.stdout, name) || strings.Contains(got.stderr, name) {
			t.Fatalf("%s was judged: %q %q", name, got.stdout, got.stderr)
		}
	}
}
