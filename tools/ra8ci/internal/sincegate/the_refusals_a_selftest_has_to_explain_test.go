// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The self-test proves this gate still reads what it claims to read, and it
// can refuse for three reasons an operator cannot see from the exit status
// alone: its fixtures could not be staged, the two checks disagreed with
// what the fixtures were built to show, or the derived scope reached
// vendored code. None of those refusals had ever run, and two of them
// returned without writing a word, which is the refusal an operator
// retries forever. They now say which step failed.

func withPublicDeclaration(t *testing.T, replacement *regexp.Regexp) {
	t.Helper()
	original := publicDecl
	publicDecl = replacement
	t.Cleanup(func() { publicDecl = original })
}

func withExcludedPrefixes(t *testing.T, replacement []string) {
	t.Helper()
	original := excludedPrefixes
	excludedPrefixes = replacement
	t.Cleanup(func() { excludedPrefixes = original })
}

// The fixtures are staged under the system temporary directory. A box that
// cannot give the gate one has to be told apart from a gate that failed its
// own checks, so the refusal names the staging step.
func TestASelfTestThatCannotStageItsFixturesSaysSo(t *testing.T) {
	root := plantRepo(t, "1.4.0", firstParty)
	t.Setenv("TMPDIR", filepath.Join(t.TempDir(), "no-such-directory"))

	got := gate(t, root, "--selftest")
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("unstageable fixtures = %+v", got)
	}
	if !strings.Contains(got.stderr, "selftest fixtures:") {
		t.Fatalf("the refusal did not name the staging step: %q", got.stderr)
	}
	if strings.Contains(got.stderr, "scope selftest") || strings.Contains(got.stderr, "check selftest") {
		t.Fatalf("a staging failure was reported as a failed check: %q", got.stderr)
	}
}

// Every missing-tag verdict is read through the declaration pattern, so a
// pattern that stops recognizing a public declaration reads a header with
// no @since anywhere as fully documented. That is the quietest way this
// gate can fail, and the self-test is what stands between it and a green
// run: it has to refuse, and name the three counts it actually saw.
func TestASelfTestWithABlindDeclarationReaderFails(t *testing.T) {
	root := plantRepo(t, "1.4.0", firstParty)
	withPublicDeclaration(t, regexp.MustCompile(`^ra8_never_declared_this_way\b`))

	got := gate(t, root, "--selftest")
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("blind declaration reader = %+v", got)
	}
	if !strings.Contains(got.stderr, "check selftest:") || !strings.Contains(got.stderr, "untagged=0") {
		t.Fatalf("the refusal did not name the check that went blind: %q", got.stderr)
	}
}

// The scope half. Vendored code carries its upstream's tags, or none at
// all, so a scope that reaches it would bury the gate in findings nobody
// here can fix. The exclusion list is the only thing keeping it out, and
// the self-test refuses the moment it stops working.
func TestASelfTestThatSeesVendoredCodeInScopeFails(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{
		"tools/ra8ci/probe.c":     "/** @since 1.4.0 */\n",
		"libs/third_party/soup.c": "/** @since 0.0.1 */\n",
	})
	withExcludedPrefixes(t, nil)

	got := gate(t, root, "--selftest")
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("vendored code in scope = %+v", got)
	}
	if !strings.Contains(got.stderr, "scope selftest: tools=true vendored=true") {
		t.Fatalf("the refusal did not name both halves of what it saw: %q", got.stderr)
	}
}

// The control: the same tree with the exclusion list as shipped passes, so
// the refusal above is the list being emptied rather than the fixture
// tripping some other guard.
func TestTheSameTreeWithTheShippedExclusionsPasses(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{
		"tools/ra8ci/probe.c":     "/** @since 1.4.0 */\n",
		"libs/third_party/soup.c": "/** @since 0.0.1 */\n",
	})

	got := gate(t, root, "--selftest")
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("shipped exclusions = %+v", got)
	}
	if !strings.Contains(got.stdout, "selftest passed") {
		t.Fatalf("a passing self-test did not say so: %q", got.stdout)
	}
}

// Staging failures and check failures are different states, and the
// version is read before either: a box with no writable temporary
// directory must still be refused on its unreadable VERSION first, so the
// order the self-test works in stays visible in what it prints.
func TestTheVersionIsReadBeforeAnyFixtureIsStaged(t *testing.T) {
	root := plantRepo(t, "1.4.0", firstParty)
	if err := os.Remove(filepath.Join(root, "VERSION")); err != nil {
		t.Fatal(err)
	}
	t.Setenv("TMPDIR", filepath.Join(t.TempDir(), "no-such-directory"))

	got := gate(t, root, "--selftest")
	if got.code != 2 || !strings.Contains(got.stderr, "VERSION") {
		t.Fatalf("absent VERSION = %+v", got)
	}
	if strings.Contains(got.stderr, "selftest fixtures:") {
		t.Fatalf("the gate staged fixtures before it trusted the version: %q", got.stderr)
	}
}
