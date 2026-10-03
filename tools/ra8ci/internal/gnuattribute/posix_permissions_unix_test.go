//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gnuattribute

import (
	context "context"
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// A directory the process cannot enter is a failed discovery, not a
// smaller scope: silently scanning less is how a gate goes quiet.
func TestADirectoryThatCannotBeEnteredFailsDiscovery(t *testing.T) {
	root := planted(t, map[string]string{"libs/deep/a.c": "int x;\n"})
	sealed := filepath.Join(root, "libs", "deep")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o755) })
	if _, err := os.ReadDir(sealed); err == nil {
		t.Skip("this process can read a sealed directory")
	}

	if _, err := discover(root); err == nil {
		t.Fatal("a sealed directory was discovered as an empty one")
	}
	got := gate(t, context.Background(), root)
	if got.code != 2 || !strings.Contains(got.stderr, "discovery failed") {
		t.Fatalf("sealed sweep = %+v", got)
	}
}
