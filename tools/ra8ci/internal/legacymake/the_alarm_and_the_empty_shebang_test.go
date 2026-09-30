// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

import (
	"regexp"
	"strings"
	"testing"
)

// A shebang line that names no interpreter is not a shell shebang. The loop
// that reads the interpreter out of it has to answer for the case where it
// never gets a word to judge, otherwise a suffixless file carrying a bare
// "#!" would fall through as whatever the last judgement happened to be.
func TestAShebangNamingNoInterpreterIsNotAShell(t *testing.T) {
	for _, head := range []string{
		"#!",
		"#!\n",
		"#!   \n",
		"#! -x\n",             // every word an option, so none is a name
		"#! --login --norc\n", // the same, spelled long
	} {
		if hasShellShebang([]byte(head)) {
			t.Errorf("hasShellShebang(%q) = true, want false", head)
		}
		// The same head decides a suffixless file, which is the only place
		// this question is ever asked of a real path.
		if runsCommands("hooks/pre-commit", []byte(head)) {
			t.Errorf("runsCommands over %q = true, want false", head)
		}
	}
}

// withActiveCommand swaps the pattern that reads a bare invocation, so the
// self-test's own assertions can be watched failing.
func withActiveCommand(t *testing.T, pattern *regexp.Regexp) {
	t.Helper()
	kept := activeCommand
	activeCommand = pattern
	t.Cleanup(func() { activeCommand = kept })
}

// The self-test is the gate's claim that its detector still reads what it
// says it reads. That claim is only worth having if a detector gone wrong
// actually breaks it, so this blinds the pattern the first case depends on
// and watches the self-test refuse.
func TestASelfTestWhoseDetectorWentBlindFails(t *testing.T) {
	withActiveCommand(t, regexp.MustCompile(`__never_written_as_an_invocation__`))

	root := plantRepo(t, map[string]string{"justfile": "ci:\n"})
	code, stdout, stderr := ranGate(t, root, "--selftest")

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	// The failure names which case broke, since a self-test that only says
	// FAIL leaves the reader to re-derive the whole table.
	if !strings.Contains(stderr, "FAIL case 1") {
		t.Fatalf("stderr = %q, want the failing case named", stderr)
	}
	// No PASS line: a run that announced both would be unreadable in CI.
	if strings.Contains(stdout, "PASS") {
		t.Fatalf("stdout = %q, want no PASS alongside the failure", stdout)
	}
}

// The self-test stops at the first broken case rather than reporting the
// whole table, so a detector blinded to a later case names that later one.
func TestASelfTestNamesTheFirstCaseThatBroke(t *testing.T) {
	// Only the comment form is blinded, so every bare and array invocation
	// ahead of it still holds and the first failure is a later case.
	kept := commentCommand
	commentCommand = regexp.MustCompile(`__never_written_as_a_comment__`)
	t.Cleanup(func() { commentCommand = kept })

	root := plantRepo(t, map[string]string{"justfile": "ci:\n"})
	code, _, stderr := ranGate(t, root, "--selftest")

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if strings.Contains(stderr, "FAIL case 1") {
		t.Fatalf("stderr = %q, want a later case named than the first", stderr)
	}
	if !strings.Contains(stderr, "FAIL case ") {
		t.Fatalf("stderr = %q, want a named failing case", stderr)
	}
}

// A self-test answers on its own terms, ahead of the scope and its floor, so
// a blinded detector is refused even over a root holding almost nothing.
func TestAFailingSelfTestIsAnsweredAheadOfTheScope(t *testing.T) {
	withActiveCommand(t, regexp.MustCompile(`__never_written_as_an_invocation__`))

	code, _, stderr := ranGate(t, t.TempDir(), "--selftest")

	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if strings.Contains(stderr, "scope collapsed") {
		t.Fatalf("stderr = %q, want the self-test refusal rather than the floor", stderr)
	}
}
