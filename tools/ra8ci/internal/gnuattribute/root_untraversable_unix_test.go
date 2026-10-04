//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	"os"
	"path/filepath"
	"testing"
)

// Discovery walks a fixed set of top-level directories. One that is absent is
// skipped, but a path that cannot be traversed must be reported. This fixture
// uses a file where a directory is expected: Unix reports ENOTDIR for child
// paths, while Windows normalizes that case to a not-found error. A Windows
// equivalent uses a DACL-denied directory fixture in
// root_untraversable_windows_test.go.
func TestARootThatCannotBeTraversedIsReportedNotSkipped(t *testing.T) {
	sealed := filepath.Join(t.TempDir(), "not-a-directory")
	if err := os.WriteFile(sealed, []byte("this is a file\n"), 0o600); err != nil {
		t.Fatalf("planting the fixture: %v", err)
	}

	files, err := discover(sealed)
	if err == nil {
		t.Fatalf("a file standing in for the repository root was read as a tree of %d files", len(files))
	}
	if files != nil {
		t.Fatalf("a failed discovery still handed back %d paths", len(files))
	}
}
