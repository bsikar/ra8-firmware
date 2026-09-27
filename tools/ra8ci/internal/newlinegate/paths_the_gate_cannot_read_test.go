// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// scan runs the gate over the named arguments and hands back the code and both
// streams, because every refusal here is judged by what the caller was told as
// much as by the status.
func scan(t *testing.T, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// sourceFile writes a first-party source file under root.
func sourceFile(t *testing.T, root, rel, body string) string {
	t.Helper()
	path := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestANamedPathThatIsNotThereIsNotAPass(t *testing.T) {
	root := t.TempDir()
	code, stdout, stderr := scan(t, root, "moved.py")
	if code != 2 {
		t.Fatalf("code = %d, want 2; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if strings.Contains(stdout, "all end in a newline") {
		t.Fatalf("a missing file was reported clean: %q", stdout)
	}
}

func TestARefusalNamesThePathAndTheReason(t *testing.T) {
	root := t.TempDir()
	_, _, stderr := scan(t, root, "moved.py")
	if !strings.Contains(stderr, "moved.py") {
		t.Fatalf("refusal does not name the path: %q", stderr)
	}
	if !strings.Contains(stderr, "cannot read") {
		t.Fatalf("refusal does not say what went wrong: %q", stderr)
	}
}

func TestAMissingPathIsRefusedWhateverItsSuffix(t *testing.T) {
	root := t.TempDir()
	for _, named := range []string{"gone.c", "gone.md", "gone"} {
		code, _, stderr := scan(t, root, named)
		if code != 2 {
			t.Fatalf("%s: code = %d, want 2 (%q)", named, code, stderr)
		}
	}
}

func TestOnePresentFileDoesNotExcuseAMissingOne(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "here.py", "ok\n")
	code, stdout, stderr := scan(t, root, "here.py", "gone.py")
	if code != 2 {
		t.Fatalf("code = %d, want 2; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if strings.Contains(stdout, "scanned") {
		t.Fatalf("the scan reported a count over an unread file: %q", stdout)
	}
}

func TestAFileTheScanCannotOpenIsRefusedNotSkipped(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "tools"), 0700); err != nil {
		t.Fatal(err)
	}
	dangling := filepath.Join(root, "tools", "linked.py")
	if err := os.Symlink(filepath.Join(root, "tools", "never-written.py"), dangling); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	sourceFile(t, root, "tools/real.py", "ok\n")
	code, stdout, stderr := scan(t, root, "tools")
	if code != 2 {
		t.Fatalf("code = %d, want 2; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stderr, "linked.py") {
		t.Fatalf("refusal does not name the unreadable file: %q", stderr)
	}
}

func TestANamedPathOutOfScopeIsStillSkippedInSilence(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "notes.md", "no newline here")
	sourceFile(t, root, "data.json", "{}")
	code, _, stderr := scan(t, root, "notes.md", "data.json")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stderr=%q", code, stderr)
	}
	if strings.Contains(stderr, "cannot read") {
		t.Fatalf("a readable out-of-scope file was refused: %q", stderr)
	}
}

func TestAnExcludedFileNamedDirectlyIsStillSkipped(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "libs/third_party/vendor.py", "no newline")
	code, _, stderr := scan(t, root, "libs/third_party/vendor.py")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stderr=%q", code, stderr)
	}
}

func TestFindingsStillOutrankNothing(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "bad.py", "missing")
	code, _, stderr := scan(t, root, "bad.py")
	if code != 1 {
		t.Fatalf("code = %d, want 1; stderr=%q", code, stderr)
	}
	if !strings.Contains(stderr, "bad.py") {
		t.Fatalf("finding not named: %q", stderr)
	}
}

func TestAnEmptyFileIsStillReadAndStillPasses(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "empty.py", "")
	code, stdout, stderr := scan(t, root, "empty.py")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "1 file(s) scanned") {
		t.Fatalf("the empty file was not counted as scanned: %q", stdout)
	}
}

func TestADirectoryNamedInPlaceOfAFileIsWalkedNotRefused(t *testing.T) {
	root := t.TempDir()
	sourceFile(t, root, "tools/good.py", "ok\n")
	code, stdout, stderr := scan(t, root, "tools")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "1 file(s) scanned") {
		t.Fatalf("directory walk lost its file: %q", stdout)
	}
}
