// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test writes three fixture files into a temporary directory it has
// just created. A directory it could not create at all is already refused; the
// gap was the narrower failure where the directory arrives and the files
// cannot be written into it, which must be a refusal and not a pass earned by
// checks that never ran.

// aTemporaryDirectoryTooDeepToWriteInto returns a directory whose path is long
// enough that the self-test's own temporary directory still fits inside the
// kernel's path limit while the fixture files beneath it do not. Creating the
// directory therefore succeeds and every write into it fails.
func aTemporaryDirectoryTooDeepToWriteInto(t *testing.T) string {
	t.Helper()
	const target = 4061 // leaves room for the temp directory, not for its files
	deep := t.TempDir()
	if len(deep) >= target {
		t.Skipf("the test root is already %d characters deep", len(deep))
	}
	for len(deep) < target {
		remaining := target - len(deep) - 1
		if remaining > 100 {
			remaining = 100
		}
		deep = filepath.Join(deep, strings.Repeat("d", remaining))
	}
	if err := os.MkdirAll(deep, 0o755); err != nil {
		t.Skipf("this filesystem will not hold a %d character path: %v", len(deep), err)
	}
	return deep
}

func TestASelfTestThatCannotWriteItsFixturesFails(t *testing.T) {
	deep := aTemporaryDirectoryTooDeepToWriteInto(t)
	t.Setenv("TMPDIR", deep)

	// Sanity: a directory can still be made here, so the refusal under test is
	// the write and not the creation the other test already covers.
	made, err := os.MkdirTemp("", "ra8ci-newline-selftest-")
	if err != nil {
		t.Skipf("no temporary directory can be made at this depth: %v", err)
	}
	defer os.RemoveAll(made)
	if writeErr := os.WriteFile(filepath.Join(made, "good.py"), []byte("x = 1\n"), 0o600); writeErr == nil {
		t.Skip("this filesystem accepts the fixture path, so the write cannot be made to fail")
	}

	code, stdout, stderr := scan(t, t.TempDir(), "--selftest")
	if code != 2 {
		t.Fatalf("exit = %d, want 2: %s %s", code, stdout, stderr)
	}
	if strings.Contains(stdout, "selftest passed") {
		t.Fatalf("a self-test that could not write its fixtures reported passing: %s", stdout)
	}
}
