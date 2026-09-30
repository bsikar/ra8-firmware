// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"fmt"
	"strings"
	"testing"
)

// floorOfDocs is the cheap half of a scope the self-test will accept: enough
// text files to clear the floor, so the hook directory is the only thing left
// for it to judge.
func floorOfDocs() map[string]string {
	files := make(map[string]string, fileFloor+8)
	for i := 0; i < fileFloor; i++ {
		files[fmt.Sprintf("docs/unit%04d.md", i)] = "ascii\n"
	}
	return files
}

// A repository with no hook directory at all is not a clean tree, it is a tree
// the self-test cannot check itself against. Reading the absent directory as
// "no hooks, nothing to verify" would let the gate pass while the one scope
// rule it names had stopped applying.
func TestTheSelfTestRefusesATreeWithNoHookDirectory(t *testing.T) {
	files := floorOfDocs()
	files["scripts/build"] = "#!/bin/sh\nexit 0\n"

	held, complaint := runSelfTest(t, plantRepo(t, files))
	if held {
		t.Fatal("the self-test accepted a tree whose hook directory does not exist")
	}
	if !strings.Contains(complaint, "cannot read the git hook scripts") {
		t.Fatalf("the refusal does not name the unreadable hook directory: %s", complaint)
	}
}

// The hook directory may hold things that are not hooks. An extensionless file
// with no shebang is prose, and the self-test passes over it rather than
// counting it or demanding the scope carry it: the count is what the refusal
// reports, so a file that was never a script must not inflate it.
func TestPlainProseInsideTheHookDirectoryIsPassedOver(t *testing.T) {
	files := floorOfDocs()
	for _, script := range sevenScripts() {
		files[script] = "#!/bin/sh\nexit 0\n"
	}
	files["scripts/git/NOTES"] = "why these hooks exist, in prose\n"
	files["scripts/git/README.md"] = "hook notes\n"

	held, complaint := runSelfTest(t, plantRepo(t, files))
	if !held {
		t.Fatalf("prose beside the hooks should not disturb the self-test: %s", complaint)
	}
}

// The same prose file must not stand in for the commit hook either: with the
// hook gone, the run has to refuse and report that it checked no hooks, not
// count the prose and claim it read one.
func TestProseInTheHookDirectoryDoesNotStandInForAHook(t *testing.T) {
	files := floorOfDocs()
	files["scripts/git/NOTES"] = "why these hooks exist, in prose\n"

	held, complaint := runSelfTest(t, plantRepo(t, files))
	if held {
		t.Fatal("the self-test accepted a hook directory holding no hooks")
	}
	if !strings.Contains(complaint, "checked=0") {
		t.Fatalf("the refusal should report that no hook was read: %s", complaint)
	}
	if !strings.Contains(complaint, "commit-msg included=false") {
		t.Fatalf("the refusal should name the missing commit hook: %s", complaint)
	}
}
