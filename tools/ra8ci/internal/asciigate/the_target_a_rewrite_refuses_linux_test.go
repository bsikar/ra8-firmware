// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

// A rewrite re-stats its target after reading it, because the thing it read
// and the thing it is about to write over are not necessarily the same object.
// A pipe is the honest case: it reads like a file and is not one, so the
// rewrite has to refuse rather than write source content into it.

// aPipeCarrying returns a named pipe with body waiting in it. The writer runs
// in its own goroutine because opening either end of a pipe blocks until the
// other end is opened.
func aPipeCarrying(t *testing.T, name, body string) string {
	t.Helper()
	pipe := filepath.Join(t.TempDir(), name)
	if err := syscall.Mkfifo(pipe, 0o644); err != nil {
		t.Skipf("this filesystem will not hold a named pipe: %v", err)
	}
	go func() {
		handle, err := os.OpenFile(pipe, os.O_WRONLY, 0)
		if err != nil {
			return
		}
		_, _ = handle.WriteString(body)
		_ = handle.Close()
	}()
	return pipe
}

func TestARewriteRefusesATargetThatIsNotARegularFile(t *testing.T) {
	pipe := aPipeCarrying(t, "dash.md", "dash\u2014\n")

	count, err := process(pipe, true)
	if err == nil {
		t.Fatalf("a pipe was rewritten and answered %d", count)
	}
	if !strings.Contains(err.Error(), "target is not a regular file") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
	// The refusal answers no count, so a caller cannot read it as work done.
	if count != 0 {
		t.Fatalf("count = %d alongside the refusal", count)
	}
	if !strings.Contains(err.Error(), "dash.md") {
		t.Fatalf("the refusal does not name the target: %v", err)
	}
}

func TestJudgingTheSameTargetIsStillAllowed(t *testing.T) {
	pipe := aPipeCarrying(t, "judge.md", "dash\u2014\n")

	// Only the rewrite needs a regular file: a check is a read, and refusing
	// it would make a pipe in the scope fail a whole scan for nothing.
	count, err := process(pipe, false)
	if err != nil {
		t.Fatalf("a pipe could not be judged: %v", err)
	}
	if count != 1 {
		t.Fatalf("count = %d, want the one non-ASCII rune", count)
	}
}

func TestACleanTargetIsNeverReStattedForARewrite(t *testing.T) {
	pipe := aPipeCarrying(t, "clean.md", "already ascii\n")

	// Nothing to rewrite means the target's shape never comes up, so a clean
	// pipe passes even with the rewrite asked for.
	count, err := process(pipe, true)
	if err != nil {
		t.Fatalf("a clean pipe was refused: %v", err)
	}
	if count != 0 {
		t.Fatalf("count = %d, want none", count)
	}
}
