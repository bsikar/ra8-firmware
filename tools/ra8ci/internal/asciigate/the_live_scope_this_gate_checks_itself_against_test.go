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

// sevenScripts is a spread of extensionless scripts with the commit hook the
// self-test names among them.
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

// A hook that exists on disk and never reaches the derived scope is the
// failure the self-test exists to catch: the file still clears the floor, and
// nothing else in the run would notice that the gate stopped reading it. Here
// the hook is ignored by git, which is how a scope quietly loses a file.
func TestTheSelfTestRefusesAScopeThatOmitsAHookScript(t *testing.T) {
	files := make(map[string]string, fileFloor+3)
	for i := 0; i < fileFloor; i++ {
		files[fmt.Sprintf("docs/unit%04d.md", i)] = "ascii\n"
	}
	files["scripts/git/commit-msg"] = "#!/bin/sh\nexit 0\n"
	files["scripts/git/pre-push"] = "#!/bin/sh\nexit 0\n"
	files[".gitignore"] = "scripts/git/pre-push\n"
	held, complaint := runSelfTest(t, plantRepo(t, files))
	if held {
		t.Fatal("the self-test accepted a scope that never derived a hook on disk")
	}
	if !strings.Contains(complaint, "omits the hook script scripts/git/pre-push") {
		t.Fatalf("the refusal does not name the hook it lost: %s", complaint)
	}
}

// The commit hook is named rather than merely counted: a repository can carry
// plenty of scripts, and hooks the scope reads correctly, and still have
// stopped deriving the one file this gate exists to reach.
func TestTheSelfTestRefusesAScopeMissingTheCommitHook(t *testing.T) {
	files := make(map[string]string, fileFloor+8)
	for i := 0; i < fileFloor; i++ {
		files[fmt.Sprintf("docs/unit%04d.md", i)] = "ascii\n"
	}
	files["scripts/git/pre-push"] = "#!/bin/sh\nexit 0\n"
	for _, script := range sevenScripts()[1:] {
		files[script] = "#!/bin/sh\nexit 0\n"
	}
	held, complaint := runSelfTest(t, plantRepo(t, files))
	if held {
		t.Fatal("the self-test accepted a scope that never derived the commit hook")
	}
	if !strings.Contains(complaint, "commit-msg included=false") {
		t.Fatalf("the refusal does not name the missing hook: %s", complaint)
	}
	if !strings.Contains(complaint, "checked=1") {
		t.Fatalf("the refusal should report the hooks it did read: %s", complaint)
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
