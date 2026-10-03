//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// One unreadable file on either side is the same problem: the gate stops
// and names which file it could not read, rather than scanning what was
// left and calling the answer complete.
func TestRunNamesTheFileItCouldNotRead(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("a sealed file is readable as root")
	}
	for _, sealed := range []struct {
		name, path, says string
	}{
		{"a source", filepath.Join("libs", "ra8_nsc", "src", "open.c"), "cannot read source open.c"},
		{"a header", filepath.Join("libs", "ra8_nsc", "inc", "ra8_nsc.h"), "cannot read header"},
	} {
		root := tree(t,
			map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
			map[string]string{"open.c": "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n"})
		if err := os.Chmod(filepath.Join(root, sealed.path), 0o000); err != nil {
			t.Fatal(err)
		}
		code, stdout, stderr := ran(t, root, nil)
		if code != 1 || !strings.Contains(stderr, sealed.says) {
			t.Fatalf("%s that cannot be opened = %d, stderr %q", sealed.name, code, stderr)
		}
		if strings.Contains(stdout, "PASS") {
			t.Fatalf("%s that cannot be opened still passed the boundary", sealed.name)
		}
	}
}
