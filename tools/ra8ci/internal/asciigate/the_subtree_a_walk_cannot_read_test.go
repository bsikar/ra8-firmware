// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A subtree the walk cannot read ends the walk rather than being passed over
// as a subtree holding nothing. Passed over, the gate would report a clean
// scan of a tree it never finished reading, and the count it prints beside
// that verdict would be the only clue.
func TestASubtreeTheWalkCannotReadEndsTheWalk(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "page.md"), []byte("ascii\n"), 0o600); err != nil {
		t.Fatalf("plant page: %v", err)
	}
	section := filepath.Join(root, "section")
	if err := os.Mkdir(section, 0o700); err != nil {
		t.Fatalf("plant section: %v", err)
	}
	if err := os.WriteFile(filepath.Join(section, "deep.md"), []byte("ascii\n"), 0o600); err != nil {
		t.Fatalf("plant deep page: %v", err)
	}

	if targets, err := walkTargets(root); err != nil || len(targets) != 2 {
		t.Fatalf("the readable tree walked to %v (%v), want both pages", targets, err)
	}
	sealed(t, section)

	targets, err := walkTargets(root)
	if err == nil {
		t.Fatalf("a sealed subtree walked to %v, want the walk ended", targets)
	}
	// The walk hands back what it had collected before it failed. That is
	// pinned rather than tightened, because the error travels with it and
	// the caller below refuses on the error: what would be wrong is a
	// caller that scanned the partial list and called the tree clean.
	if len(targets) != 1 {
		t.Fatalf("the failed walk handed back %v, want the one page it reached before the seal", targets)
	}

	code, out, errs := ranGate(t, root, "--check", root)
	if code != 2 {
		t.Fatalf("code = %d, want 2 (stdout %q, stderr %q)", code, out, errs)
	}
	if !strings.Contains(errs, "FATAL") {
		t.Fatalf("stderr = %q, want the run ended", errs)
	}
	if out != "" {
		t.Fatalf("stdout carried %q, want no verdict on a tree never finished", out)
	}
}
