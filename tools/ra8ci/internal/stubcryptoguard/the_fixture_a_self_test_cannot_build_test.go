// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test builds a fixture tree of its own before it judges anything, so
// every step of that build has to end in a refusal that names what failed. A
// self-test that cannot build its fixture and says nothing is worse than none:
// the scan behind it would still report a guarded tree.

// aTemporaryRootWithNoRoomForTheFixtureTree points the temporary directory at a
// path long enough that the self-test's own root still fits inside the kernel's
// path limit while the libs/fixture directories beneath it do not. The root is
// therefore created and the tree inside it cannot be.
func aTemporaryRootWithNoRoomForTheFixtureTree(t *testing.T) string {
	t.Helper()
	// The root lands at this depth plus the pattern and its random suffix,
	// which leaves it inside the limit for every suffix length while
	// libs/fixture beneath it is over the limit for all of them.
	const target = 4061
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

func TestASelfTestThatCannotBuildItsFixtureTreeSaysSo(t *testing.T) {
	deep := aTemporaryRootWithNoRoomForTheFixtureTree(t)
	t.Setenv("TMPDIR", deep)

	// Sanity: a root can still be made here, so the refusal under test is the
	// fixture tree inside it and not the root itself.
	root, err := os.MkdirTemp("", "stub-crypto-selftest-")
	if err != nil {
		t.Skipf("no temporary root can be made at this depth: %v", err)
	}
	defer os.RemoveAll(root)
	if mkErr := os.MkdirAll(filepath.Join(root, "libs", "fixture"), 0o700); mkErr == nil {
		t.Skip("this filesystem accepts the fixture path, so the build cannot be made to fail")
	}

	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr)
	// A self-test that could not run answers on the self-test's own terms,
	// the same 1 a failing case gives, never a pass.
	if code != 1 {
		t.Fatalf("exit = %d, want 1: stdout %q, stderr %q", code, stdout.String(), stderr.String())
	}
	// The operator has to be told which step failed, not just that something did.
	if !strings.Contains(stderr.String(), "create self-test fixture") {
		t.Fatalf("the refusal does not name the fixture build: %q", stderr.String())
	}
	if strings.Contains(stdout.String(), "PASS") {
		t.Fatalf("a self-test that built nothing reported passing: %q", stdout.String())
	}
}
