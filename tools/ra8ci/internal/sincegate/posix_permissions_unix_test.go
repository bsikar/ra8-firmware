//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// Both checks read the file themselves, and a file neither can read yields no
// findings. That is what keeps a path the sweep listed but the filesystem will
// not hand over from being reported as a missing tag against its author.
func TestAFileNeitherCheckCanReadYieldsNoFindings(t *testing.T) {
	dir := t.TempDir()
	sealed := header(t, dir, "sealed.h", strings.Join([]string{
		"/** @brief no tag above this one, and a wrong value below. */",
		declared("ra8_thing_start"),
		"/** @since 9.9.9 */",
	}, "\n")+"\n")
	if problems := checkPresence(sealed); len(problems) != 1 {
		t.Fatalf("the readable header did not report its missing tag: %v", problems)
	}
	if problems := checkValues(sealed, "1.4.0"); len(problems) != 1 {
		t.Fatalf("the readable header did not report its stale value: %v", problems)
	}

	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o600) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this process can read a sealed file")
	}
	if problems := checkPresence(sealed); problems != nil {
		t.Fatalf("a sealed header was reported against: %v", problems)
	}
	if problems := checkValues(sealed, "1.4.0"); problems != nil {
		t.Fatalf("a sealed header was reported against: %v", problems)
	}

	absent := filepath.Join(dir, "libs", "ra8_thing", "inc", "never_written.h")
	if problems := checkPresence(absent); problems != nil {
		t.Fatalf("an absent header was reported against: %v", problems)
	}
	if problems := checkValues(absent, "1.4.0"); problems != nil {
		t.Fatalf("an absent header was reported against: %v", problems)
	}
}
