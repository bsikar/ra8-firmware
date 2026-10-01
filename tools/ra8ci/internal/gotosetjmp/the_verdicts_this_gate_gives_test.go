// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package gotosetjmp

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// plantSourceRepo plants the files and makes the root a Git repository. The
// gate runs `git ls-files --cached --others --exclude-standard`, so untracked
// files answer and the fixture needs neither a commit nor a configured
// identity.
func plantSourceRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not available on this box")
	}
	root := t.TempDir()
	for rel, contents := range files {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("mkdir for %s: %v", rel, err)
		}
		if err := os.WriteFile(full, []byte(contents), 0o644); err != nil {
			t.Fatalf("write %s: %v", rel, err)
		}
	}
	if out, err := exec.Command("git", "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git init: %v: %s", err, out)
	}
	return root
}

// ranGate runs the gate over root and answers its exit status and both streams.
func ranGate(t *testing.T, ctx context.Context, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestRunCountsTheFilesItScannedWhenTheTreeIsClean(t *testing.T) {
	root := plantSourceRepo(t, map[string]string{
		"libs/ra8_core/clock.c": "void tick(void) { for (int i = 0; i < 1; ++i) {} }\n",
		"libs/ra8_core/clock.h": "void tick(void);\n",
		"docs/notes.md":         "goto setjmp longjmp\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root)
	if code != 0 {
		t.Fatalf("exit = %d, want 0; stderr = %q", code, stderr)
	}
	if stdout != "ra8ci no-goto-setjmp: PASS -- 2 file(s) scanned, 0 findings.\n" {
		t.Fatalf("stdout = %q", stdout)
	}
	if stderr != "" {
		t.Fatalf("a clean run wrote to stderr: %q", stderr)
	}
}

func TestRunNamesEveryBannedTokenAndTotalsThem(t *testing.T) {
	root := plantSourceRepo(t, map[string]string{
		"libs/ra8_core/retry.c": "void f(int *buf)\n{\n    goto done;\ndone:\n    setjmp(buf);\n}\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root)
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a failing run wrote to stdout: %q", stdout)
	}
	for _, want := range []string{
		"libs/ra8_core/retry.c:3: `goto` is banned",
		"libs/ra8_core/retry.c:5: `setjmp` is banned",
		"NASA Power-of-10 Rule 1",
		"ra8ci no-goto-setjmp: 2 banned control-flow token(s) found.\n",
	} {
		if !strings.Contains(stderr, want) {
			t.Fatalf("stderr omits %q: %q", want, stderr)
		}
	}
}

// A file the gate cannot read is a failure, not a skip: a gate that passed
// over what it failed to open would report a tree clean it never scanned.
func TestRunRefusesASourceFileItCannotRead(t *testing.T) {
	root := plantSourceRepo(t, map[string]string{"libs/ra8_core/sealed.c": "void f(void) {}\n"})
	sealed := filepath.Join(root, "libs", "ra8_core", "sealed.c")
	if err := os.Chmod(sealed, 0o000); err != nil {
		t.Fatalf("chmod: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o644) })
	if _, err := os.ReadFile(sealed); err == nil {
		t.Skip("this box reads a mode 0o000 file")
	}
	code, stdout, stderr := ranGate(t, context.Background(), root)
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a refused run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "cannot read libs/ra8_core/sealed.c") {
		t.Fatalf("stderr does not name the unreadable file: %q", stderr)
	}
}

func TestRunRefusesADirectoryThatIsNotARepository(t *testing.T) {
	root := t.TempDir()
	code, stdout, stderr := ranGate(t, context.Background(), root)
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a refused run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "enumerate repository sources") {
		t.Fatalf("stderr = %q, want the enumeration failure", stderr)
	}
}

// A cancelled run says it was cancelled rather than blaming Git for a failure
// the caller caused.
func TestRunSaysACancelledEnumerationWasCancelled(t *testing.T) {
	root := plantSourceRepo(t, map[string]string{"libs/ra8_core/clock.c": "void f(void) {}\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	code, stdout, stderr := ranGate(t, ctx, root)
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a cancelled run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "cancelled") {
		t.Fatalf("stderr = %q, want the cancellation", stderr)
	}
}

// The build-output rule is not "anywhere named build". A vendor directory of
// our own under libs/ is first-party source and stays in scope; the same
// directory name under a build tree root does not.
func TestBuildOutputPathsAreJudgedByTheirTopLevel(t *testing.T) {
	for _, item := range []struct {
		rel  string
		want bool
	}{
		{"build/unit.c", true},
		{"apps/build/unit.c", true},
		{"tools/cmake-build-debug/unit.c", true},
		{"port/build_host/unit.c", true},
		{"libs/build/unit.c", false},
		{"libs/deep/build-debug/unit.c", false},
		{"libs/CMakeFiles/unit.c", true},
		{"libs/ra8_core/_deps/unit.c", true},
		{"libs/ra8_core/__pycache__/unit.c", true},
		{"libs/node_modules/unit.c", true},
		{"libs/build.c", false},
		{"libs/ra8_core/clock.c", false},
	} {
		if got := isBuildOutputPath(item.rel); got != item.want {
			t.Errorf("isBuildOutputPath(%q) = %v, want %v", item.rel, got, item.want)
		}
	}
}

// Line numbers have to survive every line ending a source file can carry, or
// the report points an author at the wrong line.
func TestLinesAreCountedAcrossEveryLineEnding(t *testing.T) {
	for name, text := range map[string]string{
		"unix":             "void f(void)\n{\n    goto done;\n}\n",
		"windows":          "void f(void)\r\n{\r\n    goto done;\r\n}\r\n",
		"classic mac":      "void f(void)\r{\r    goto done;\r}\r",
		"no final newline": "void f(void)\n{\n    goto done;\n}",
	} {
		found := scanText(text)
		if len(found) != 1 {
			t.Fatalf("%s: findings = %+v, want one", name, found)
		}
		if found[0].line != 3 || found[0].token != "goto" {
			t.Errorf("%s: finding = %+v, want goto on line 3", name, found[0])
		}
	}
}

func TestEmptyTextHasNoLinesAndNoFindings(t *testing.T) {
	if lines := splitLines(""); lines != nil {
		t.Fatalf("splitLines(\"\") = %#v, want nil", lines)
	}
	if found := scanText(""); len(found) != 0 {
		t.Fatalf("scanText(\"\") = %+v", found)
	}
}
