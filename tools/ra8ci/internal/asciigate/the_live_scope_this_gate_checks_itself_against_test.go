// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"bytes"
	"context"
	"fmt"
	"strings"
	"testing"
)

// liveScope plants a repository large enough for the self-test to accept it:
// fileFloor text files, plus the extensionless scripts it insists on finding.
// Empty bodies are enough, since the self-test judges the scope it derives
// rather than what the files hold.
func liveScope(t *testing.T, scripts []string) string {
	t.Helper()
	files := make(map[string]string, fileFloor+len(scripts))
	for i := 0; i < fileFloor; i++ {
		files[fmt.Sprintf("docs/unit%04d.md", i)] = "ascii\n"
	}
	for _, script := range scripts {
		files[script] = "#!/bin/sh\nexit 0\n"
	}
	return plantRepo(t, files)
}

// sevenScripts is the smallest set the self-test accepts, with the commit hook
// it names among them.
func sevenScripts() []string {
	return []string{
		"scripts/git/commit-msg",
		"scripts/build", "scripts/flash", "scripts/lint",
		"scripts/release", "scripts/sync", "scripts/verify",
	}
}

func runSelfTest(t *testing.T, root string) (bool, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	held := selfTest(context.Background(), root, &stdout, &stderr)
	return held, stderr.String()
}

// The self-test is what stands between this gate and a scan it cannot trust,
// so the case that matters most is the one where it accepts: a scope over the
// floor, carrying the extensionless scripts it expects and the commit hook it
// names. Without this, every other self-test case only pins a refusal.
func TestTheSelfTestHoldsOnAScopeThatMeetsItsOwnTerms(t *testing.T) {
	held, complaint := runSelfTest(t, liveScope(t, sevenScripts()))
	if !held {
		t.Fatalf("the self-test refused a scope on its own terms: %s", complaint)
	}
	if complaint != "" {
		t.Fatalf("a self-test that held still complained: %s", complaint)
	}
}

// Seven is a floor, not a target. A repository that has lost its scripts still
// clears the file floor, so the count is the only thing that catches it.
func TestTheSelfTestRefusesAScopeThatHasLostItsScripts(t *testing.T) {
	scripts := sevenScripts()[:6]
	held, complaint := runSelfTest(t, liveScope(t, scripts))
	if held {
		t.Fatal("the self-test accepted a scope holding six extensionless scripts")
	}
	if !strings.Contains(complaint, "extensionless derived scope has 6 entries") {
		t.Fatalf("the refusal does not name the count it counted: %s", complaint)
	}
}

// The commit hook is named rather than counted: a scope can hold plenty of
// scripts and still have stopped deriving the one the gate exists to reach.
func TestTheSelfTestRefusesAScopeMissingTheCommitHook(t *testing.T) {
	scripts := []string{
		"scripts/build", "scripts/flash", "scripts/lint", "scripts/release",
		"scripts/sync", "scripts/verify", "scripts/package",
	}
	held, complaint := runSelfTest(t, liveScope(t, scripts))
	if held {
		t.Fatal("the self-test accepted a scope that never derived the commit hook")
	}
	if !strings.Contains(complaint, "commit-msg included=false") {
		t.Fatalf("the refusal does not name the missing hook: %s", complaint)
	}
	if !strings.Contains(complaint, "has 7 entries") {
		t.Fatalf("the refusal should still report the count it held: %s", complaint)
	}
}

// An extensionless file is only a script if its first line says so, so a
// plain extensionless file neither joins the count nor stands in for the hook.
func TestAnExtensionlessFileWithoutAShebangIsNotAScript(t *testing.T) {
	files := make(map[string]string, fileFloor+8)
	for i := 0; i < fileFloor; i++ {
		files[fmt.Sprintf("docs/unit%04d.md", i)] = "ascii\n"
	}
	for _, script := range sevenScripts() {
		files[script] = "#!/bin/sh\nexit 0\n"
	}
	files["scripts/NOTES"] = "not a script, just prose\n"
	held, complaint := runSelfTest(t, plantRepo(t, files))
	if !held {
		t.Fatalf("a plain extensionless file should not disturb the self-test: %s", complaint)
	}
}
