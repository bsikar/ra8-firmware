// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nullgate

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// plantFiles writes each path/content pair under a fresh root and hands the
// root back. Paths are slash-separated and relative.
func plantFiles(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for name, content := range files {
		full := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(full), 0o700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	return root
}

// plantRepo plants the files and makes the root a git repository. Discovery
// asks for --others --exclude-standard, so untracked files answer and no
// commit and no git identity are needed.
func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable")
	}
	root := plantFiles(t, files)
	if out, err := exec.Command("git", "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git init: %v: %s", err, out)
	}
	return root
}

// TestRunAnswersItsArguments pins what each invocation is worth before a single
// file is read: the usage refusal, the unknown option, the self-test, and the
// two ways an explicit path is dropped for being out of scope rather than for
// being clean.
func TestRunAnswersItsArguments(t *testing.T) {
	t.Parallel()

	root := plantFiles(t, map[string]string{
		"src/bad.c":       "int x = NULL;\n",
		"tests/stim.c":    "int x = NULL;\n",
		"notes/README.md": "NULL is fine in prose\n",
	})

	for _, tc := range []struct {
		name     string
		args     []string
		wantCode int
		wantOut  string
		wantErr  string
	}{
		{name: "no arguments", wantCode: 2, wantErr: "usage: check_no_null.py"},
		{name: "unknown option", args: []string{"-x"}, wantCode: 2, wantErr: "unknown option: -x"},
		{name: "self test", args: []string{"--selftest"}, wantCode: 0, wantOut: "selftest: all assertions held"},
		{name: "exempt directory", args: []string{"tests/stim.c"}, wantCode: 0, wantOut: "check_no_null.py: 0 findings."},
		{name: "unwatched extension", args: []string{"notes/README.md"}, wantCode: 0, wantOut: "check_no_null.py: 0 findings."},
		{name: "a watched file", args: []string{"src/bad.c"}, wantCode: 1, wantErr: "src/bad.c:1: bare NULL"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var out, errOut bytes.Buffer

			code := Run(context.Background(), root, tc.args, &out, &errOut)
			if code != tc.wantCode {
				t.Fatalf("exit %d, want %d (stdout %q, stderr %q)", code, tc.wantCode, out.String(), errOut.String())
			}
			if tc.wantOut != "" && !strings.Contains(out.String(), tc.wantOut) {
				t.Fatalf("stdout = %q, want it to carry %q", out.String(), tc.wantOut)
			}
			if tc.wantErr != "" && !strings.Contains(errOut.String(), tc.wantErr) {
				t.Fatalf("stderr = %q, want it to carry %q", errOut.String(), tc.wantErr)
			}
			if tc.wantErr == "" && errOut.Len() != 0 {
				t.Fatalf("stderr carried %q, want nothing", errOut.String())
			}
		})
	}
}

// TestRunRefusesAThinDiscovery pins the two ways --all declines to report a
// clean tree it never enumerated: a root that is no repository at all, and a
// repository holding fewer paths than the floor. Both are refusals, not a pass,
// which is the whole point of the floor.
func TestRunRefusesAThinDiscovery(t *testing.T) {
	t.Parallel()

	t.Run("not a repository", func(t *testing.T) {
		root := plantFiles(t, map[string]string{"src/bad.c": "int x = NULL;\n"})

		var out, errOut bytes.Buffer
		if code := Run(context.Background(), root, []string{"--all"}, &out, &errOut); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if !strings.Contains(errOut.String(), "discovery failed") {
			t.Fatalf("stderr = %q, want it to carry the discovery refusal", errOut.String())
		}
		if out.Len() != 0 {
			t.Fatalf("stdout carried %q, want nothing", out.String())
		}
	})

	t.Run("under the floor", func(t *testing.T) {
		root := plantRepo(t, map[string]string{"src/bad.c": "int x = NULL;\n"})

		var out, errOut bytes.Buffer
		if code := Run(context.Background(), root, []string{"--all"}, &out, &errOut); code != 2 {
			t.Fatalf("exit %d, want 2", code)
		}
		if !strings.Contains(errOut.String(), "floor is 1000") {
			t.Fatalf("stderr = %q, want it to carry the floor refusal", errOut.String())
		}
		if strings.Contains(out.String(), "0 findings") {
			t.Fatalf("stdout reported a clean tree it never enumerated: %q", out.String())
		}
	})
}

// TestRunStopsWhenTheContextIsCancelled pins that cancellation is answered
// before the file is read, and answered as a refusal rather than as a clean
// tree.
func TestRunStopsWhenTheContextIsCancelled(t *testing.T) {
	t.Parallel()

	root := plantFiles(t, map[string]string{"src/bad.c": "int x = NULL;\n"})

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	var out, errOut bytes.Buffer
	if code := Run(ctx, root, []string{"src/bad.c"}, &out, &errOut); code != 2 {
		t.Fatalf("exit %d, want 2", code)
	}
	if !strings.Contains(errOut.String(), "cancelled") {
		t.Fatalf("stderr = %q, want it to carry the cancellation", errOut.String())
	}
	if out.Len() != 0 {
		t.Fatalf("stdout carried %q, want nothing", out.String())
	}
}

// TestNeedsCheckKeepsWhatItCannotPlace pins the judgement on a path the scope
// rules cannot speak for: a watched extension outside the root is checked
// rather than quietly dropped, while an unwatched extension is dropped
// wherever it sits.
func TestNeedsCheckKeepsWhatItCannotPlace(t *testing.T) {
	t.Parallel()

	root := t.TempDir()
	outside := filepath.Join(filepath.Dir(root), "elsewhere.c")

	for _, tc := range []struct {
		name string
		path string
		want bool
	}{
		{name: "outside the root", path: outside, want: true},
		{name: "reached by a parent hop", path: "../elsewhere.c", want: true},
		{name: "inside and watched", path: "src/main.c", want: true},
		{name: "inside and exempt", path: "tests/stim.c"},
		{name: "unwatched extension outside the root", path: filepath.Join(filepath.Dir(root), "elsewhere.py")},
		// needsCheck lowercases the extension before admitting a path, but
		// inScope does not, so an uppercase .C inside the root is admitted
		// by its extension and then dropped by the scope. Outside the root
		// the scope never speaks, so the same name is checked.
		{name: "uppercase extension inside the root", path: "src/main.C"},
		{name: "uppercase extension outside the root", path: filepath.Join(filepath.Dir(root), "elsewhere.C"), want: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := needsCheck(root, tc.path); got != tc.want {
				t.Fatalf("needsCheck(%q) = %t, want %t", tc.path, got, tc.want)
			}
		})
	}
}
