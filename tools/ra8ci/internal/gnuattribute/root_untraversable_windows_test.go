//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

func TestARootThatCannotBeTraversedIsReportedNotSkipped(t *testing.T) {
	root := t.TempDir()
	sealed := filepath.Join(root, "libs")
	if err := os.Mkdir(sealed, 0o700); err != nil {
		t.Fatalf("planting the fixture: %v", err)
	}
	if err := os.WriteFile(filepath.Join(sealed, "probe.c"), []byte("int probe;\n"), 0o600); err != nil {
		t.Fatalf("planting a source file: %v", err)
	}
	if err := testprivatefile.DenyDirectoryRead(sealed); err != nil {
		t.Fatalf("denying directory listing: %v", err)
	}
	t.Cleanup(func() {
		if err := testprivatefile.RestoreDirectory(sealed); err != nil {
			t.Errorf("restoring directory ACL: %v", err)
		}
	})

	files, err := discover(root)
	if err == nil {
		t.Fatalf("a denied root was read as a tree of %d files", len(files))
	}
	if files != nil {
		t.Fatalf("a failed discovery still handed back %d paths", len(files))
	}
}
