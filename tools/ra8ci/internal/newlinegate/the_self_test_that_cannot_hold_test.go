// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test is what says the gate is still worth trusting before a sweep
// is believed. Every way it can fail has to end in a refusal that names what
// was missing, because a self-test that fails quietly is worse than no
// self-test: the sweep behind it would still report a clean tree.

// A scope of plenty, so the floors are cleared and the checks past them are
// the ones under test. Nothing here ends without a newline, so the sweep
// itself has nothing to say.
func scopeOfPlenty(t *testing.T, extra map[string]string) string {
	t.Helper()
	files := make(map[string]string, fileFloor+len(extra))
	for index := 0; index < fileFloor+20; index++ {
		files["src/unit"+digits(index)+".py"] = "x = 1\n"
	}
	for rel, body := range extra {
		files[rel] = body
	}
	return plantRepo(t, files)
}

// The self-test builds its fixtures in a temporary directory. With nowhere to
// build them it fails rather than passing on the strength of the checks it
// never ran.
func TestASelfTestWithNowhereToBuildItsFixturesFails(t *testing.T) {
	t.Setenv("TMPDIR", filepath.Join(t.TempDir(), "not-there"))

	code, stdout, stderr := scan(t, t.TempDir(), "--selftest")
	if code != 2 {
		t.Fatalf("exit = %d, want 2: %s %s", code, stdout, stderr)
	}
	if strings.Contains(stdout, "selftest passed") {
		t.Fatalf("a self-test that could not build anything reported passing: %s", stdout)
	}
}

// The self-test derives the real scope as its last check, so a root it cannot
// derive a scope from is a refusal that names the floor it fell through.
func TestASelfTestOverATreeWithNoScopeNamesTheFloor(t *testing.T) {
	code, stdout, stderr := scan(t, plantRepo(t, map[string]string{"src/one.py": "x = 1\n"}), "--selftest")
	if code != 2 {
		t.Fatalf("exit = %d, want 2: %s %s", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "floor is") {
		t.Fatalf("the refusal does not name the floor: %q", stderr)
	}
	if strings.Contains(stdout, "selftest passed") {
		t.Fatalf("a collapsed scope reported a passing self-test: %s", stdout)
	}
}

// Past the floor, the self-test still checks that the scope reaches the two
// roots the gate exists to cover. A scope of the right size drawn from the
// wrong part of the tree is the failure this catches, and it reports which
// root was missing rather than only that something was.
func TestASelfTestWhoseScopeMissesTheRootsNamesThem(t *testing.T) {
	code, stdout, stderr := scan(t, scopeOfPlenty(t, nil), "--selftest")
	if code != 2 {
		t.Fatalf("exit = %d, want 2: %s %s", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "just=false") || !strings.Contains(stderr, "infra=false") {
		t.Fatalf("the refusal does not say which roots were missing: %q", stderr)
	}
	if strings.Contains(stdout, "selftest passed") {
		t.Fatalf("a scope missing both roots reported a passing self-test: %s", stdout)
	}
}

// Both roots present is still not enough. The gate's whole reason to look at
// extensionless files is the hooks, so a scope holding no scripts and not the
// commit-msg hook is refused with both counts, which is what tells an author
// whether the scope shrank or the hook simply moved.
func TestASelfTestWhoseScopeHoldsNoScriptsNamesTheHook(t *testing.T) {
	root := scopeOfPlenty(t, map[string]string{
		"just/build.py":  "x = 1\n",
		"infra/apply.py": "x = 1\n",
	})

	code, stdout, stderr := scan(t, root, "--selftest")
	if code != 2 {
		t.Fatalf("exit = %d, want 2: %s %s", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "extensionless script(s)") || !strings.Contains(stderr, "commit-msg included=false") {
		t.Fatalf("the refusal does not name the scripts and the hook: %q", stderr)
	}
	if strings.Contains(stdout, "selftest passed") {
		t.Fatalf("a scope with no scripts reported a passing self-test: %s", stdout)
	}
}

// The same derivation, run directly, is what the checks above are reading.
// Pinned so a future change to the self-test cannot quietly stop deriving a
// scope at all and still look like it is testing one.
func TestTheSelfTestReadsARealDerivedScope(t *testing.T) {
	targets, err := derivedTargets(context.Background(), scopeOfPlenty(t, nil))
	if err != nil {
		t.Fatalf("a planted scope of plenty could not be derived: %v", err)
	}
	if len(targets) < fileFloor {
		t.Fatalf("the planted scope holds %d file(s), under the floor of %d", len(targets), fileFloor)
	}
}
