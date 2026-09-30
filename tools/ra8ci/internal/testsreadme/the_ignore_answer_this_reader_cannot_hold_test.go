// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bufio"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The ignore carve-out is read line by line out of what git check-ignore
// answers, and the reader holds one line at a time. These pin what happens at
// the edge of what it can hold, because a silently short read would be the
// worst of the two failures: the ignore set would come back missing names, and
// the gate would then demand a README row for a directory the developer has
// told Git to ignore.

// A repository whose ignore rules match everything handed to them, so whatever
// goes in on stdin comes straight back out on stdout.
func ignoringEverything(t *testing.T) string {
	t.Helper()
	if _, err := os.Stat(trustedGit); err != nil {
		t.Skipf("trusted git is unavailable: %v", err)
	}
	root := t.TempDir()
	if output, err := exec.Command(trustedGit, "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git init unavailable: %v: %s", err, output)
	}
	if err := os.WriteFile(filepath.Join(root, ".gitignore"), []byte("*\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	return root
}

// One answer line longer than the reader's own buffer is refused by name, and
// the caller is handed no ignore set at all rather than a truncated one.
func TestAnIgnoreAnswerTooLongForTheReaderIsRefused(t *testing.T) {
	root := ignoringEverything(t)
	name := strings.Repeat("a", bufio.MaxScanTokenSize+4464)

	ignored, err := ignoredNames(context.Background(), root, map[string]struct{}{name: {}}, sanitizedGitEnvironment(os.Environ()))
	if err == nil {
		t.Fatalf("an answer line of %d bytes was read whole: %d name(s) ignored", len(name), len(ignored))
	}
	if !strings.Contains(err.Error(), "read git check-ignore output") {
		t.Fatalf("the refusal does not name the read it failed: %v", err)
	}
	if ignored != nil {
		t.Fatalf("a refused read still handed back an ignore set: %v", ignored)
	}
}

// The same answer one byte inside the buffer is read and honoured, which is
// what says the refusal above is about the line the reader could not hold and
// not about long answers in general.
func TestAnIgnoreAnswerThatJustFitsIsStillHonoured(t *testing.T) {
	root := ignoringEverything(t)
	name := strings.Repeat("a", bufio.MaxScanTokenSize-1)

	ignored, err := ignoredNames(context.Background(), root, map[string]struct{}{name: {}}, sanitizedGitEnvironment(os.Environ()))
	if err != nil {
		t.Fatalf("an answer line of %d bytes was refused: %v", len(name), err)
	}
	if _, held := ignored[name]; !held || len(ignored) != 1 {
		t.Fatalf("the ignored name did not survive the read: %d name(s)", len(ignored))
	}
}
