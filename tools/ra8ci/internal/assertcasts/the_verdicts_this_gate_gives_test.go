// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package assertcasts

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// plantTree writes the files under a fresh root and answers it.
func plantTree(t *testing.T, files map[string]string) string {
	t.Helper()
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
	return root
}

// ranGate runs the gate over root and answers its exit status and both streams.
func ranGate(t *testing.T, ctx context.Context, root string, args ...string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// The findings are the gate's whole output, so where each half of them goes
// matters: an author reads the list on stdout and CI reads the count.
func TestRunWritesFindingsToStdoutAndTheCountToStderr(t *testing.T) {
	root := plantTree(t, map[string]string{
		"tests/unit/a.c": "TEST_ASSERT_EQ((int)a, b);\nTEST_ASSERT_EQ(a, (size_t)b);\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root, "tests/unit/a.c")
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	for _, want := range []string{
		"tests/unit/a.c:1: cast in first arg of TEST_ASSERT_EQ: TEST_ASSERT_EQ((int)a...",
		"tests/unit/a.c:2: cast in second arg of TEST_ASSERT_EQ: ...(size_t)b",
	} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("stdout omits %q: %q", want, stdout)
		}
	}
	if !strings.Contains(stderr, "2 redundant cast(s) in TEST_ASSERT_EQ.") {
		t.Fatalf("stderr omits the count: %q", stderr)
	}
	if !strings.Contains(stderr, "Remove the redundant casts before retrying.") {
		t.Fatalf("stderr omits the remedy: %q", stderr)
	}
}

func TestRunSaysNothingAboutACleanFile(t *testing.T) {
	root := plantTree(t, map[string]string{
		"tests/unit/clean.c": "TEST_ASSERT_EQ(a, b);\nTEST_ASSERT_EQ(count, expected);\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root, "tests/unit/clean.c")
	if code != 0 {
		t.Fatalf("exit = %d, want 0; stderr = %q", code, stderr)
	}
	if stdout != "" || stderr != "" {
		t.Fatalf("a clean run wrote stdout %q stderr %q", stdout, stderr)
	}
}

// --all walks tests/ only, and it reaches every depth of it.
func TestDiscoverTakesEveryTestSourceAndNothingElse(t *testing.T) {
	root := plantTree(t, map[string]string{
		"tests/unit/a.c":            "TEST_ASSERT_EQ((int)a, b);\n",
		"tests/unit/deep/b.c":       "TEST_ASSERT_EQ((size_t)a, b);\n",
		"tests/unit/notes.md":       "TEST_ASSERT_EQ((int)a, b)\n",
		"tests/unit/header.h":       "TEST_ASSERT_EQ((int)a, b);\n",
		"libs/ra8_core/elsewhere.c": "TEST_ASSERT_EQ((int)a, b);\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root, "--all")
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	for _, want := range []string{"a.c:1:", "b.c:1:"} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("stdout omits %q: %q", want, stdout)
		}
	}
	for _, unwanted := range []string{"notes.md", "header.h", "elsewhere.c"} {
		if strings.Contains(stdout, unwanted) {
			t.Fatalf("stdout reaches outside the test sources (%s): %q", unwanted, stdout)
		}
	}
	if !strings.Contains(stderr, "2 redundant cast(s)") {
		t.Fatalf("stderr = %q", stderr)
	}
}

// A repository with no tests directory is a discovery failure, not a clean
// tree: a gate that answered 0 there would pass a checkout it never read.
func TestRunRefusesWhenThereIsNothingToDiscover(t *testing.T) {
	root := plantTree(t, map[string]string{"libs/ra8_core/clock.c": "void f(void) {}\n"})
	code, stdout, stderr := ranGate(t, context.Background(), root, "--all")
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a refused run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "discovery failed") {
		t.Fatalf("stderr = %q, want the discovery failure", stderr)
	}
}

func TestRunRefusesAnInvocationItCannotHonour(t *testing.T) {
	root := plantTree(t, map[string]string{"tests/unit/a.c": "TEST_ASSERT_EQ(a, b);\n"})
	for name, item := range map[string]struct {
		args []string
		code int
		says string
	}{
		"no arguments at all":      {nil, 1, "usage: ra8ci assert-casts"},
		"an unknown option":        {[]string{"-x"}, 2, "unknown or incompatible arguments"},
		"an option beside a file":  {[]string{"tests/unit/a.c", "--all"}, 2, "unknown or incompatible arguments"},
		"the self-test beside one": {[]string{"--selftest", "tests/unit/a.c"}, 2, "unknown or incompatible arguments"},
	} {
		code, stdout, stderr := ranGate(t, context.Background(), root, item.args...)
		if code != item.code {
			t.Errorf("%s: exit = %d, want %d; stderr = %q", name, code, item.code, stderr)
		}
		if stdout != "" {
			t.Errorf("%s: wrote to stdout: %q", name, stdout)
		}
		if !strings.Contains(stderr, item.says) {
			t.Errorf("%s: stderr = %q, want %q", name, stderr, item.says)
		}
	}
}

func TestRunRefusesAFileItCannotRead(t *testing.T) {
	root := plantTree(t, nil)
	code, stdout, stderr := ranGate(t, context.Background(), root, "tests/unit/absent.c")
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a refused run wrote to stdout: %q", stdout)
	}
	if !strings.Contains(stderr, "cannot read tests/unit/absent.c") {
		t.Fatalf("stderr does not name the file: %q", stderr)
	}
}

func TestRunTakesAnAbsolutePathAsGiven(t *testing.T) {
	root := plantTree(t, map[string]string{"tests/unit/a.c": "TEST_ASSERT_EQ((int)a, b);\n"})
	absolute := filepath.Join(root, "tests", "unit", "a.c")
	code, stdout, stderr := ranGate(t, context.Background(), root, absolute)
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if !strings.Contains(stdout, absolute+":1: cast in first arg") {
		t.Fatalf("stdout = %q, want the path as given", stdout)
	}
}

func TestRunRefusesACancelledScan(t *testing.T) {
	root := plantTree(t, map[string]string{"tests/unit/a.c": "TEST_ASSERT_EQ((int)a, b);\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	code, stdout, stderr := ranGate(t, ctx, root, "tests/unit/a.c")
	if code != 2 {
		t.Fatalf("exit = %d, want 2; stderr = %q", code, stderr)
	}
	if stdout != "" {
		t.Fatalf("a cancelled run reported findings: %q", stdout)
	}
	if !strings.Contains(stderr, "cancelled") {
		t.Fatalf("stderr = %q, want the cancellation", stderr)
	}
}

// Bytes that are not ASCII are replaced rather than refused, so a source file
// carrying a non-ASCII comment is still judged on the code around it, and the
// replacement never shifts the line a finding is reported at.
func TestNonASCIIBytesAreReplacedAndTheLineStillHolds(t *testing.T) {
	root := plantTree(t, map[string]string{
		"tests/unit/a.c": "/* \xc2\xb5s of delay */\nTEST_ASSERT_EQ((int)a, b);\n",
	})
	code, stdout, stderr := ranGate(t, context.Background(), root, "tests/unit/a.c")
	if code != 1 {
		t.Fatalf("exit = %d, want 1; stderr = %q", code, stderr)
	}
	if !strings.Contains(stdout, "tests/unit/a.c:2: cast in first arg") {
		t.Fatalf("stdout = %q, want the finding on line 2", stdout)
	}
}

// A long argument is cut to 60 runes, and the cut is on a rune boundary, so a
// multi-byte character never comes out as half of itself.
func TestALongArgumentIsCutOnARuneBoundary(t *testing.T) {
	long := "(int)" + strings.Repeat("\u00e9", 80)
	if got := trunc(long, 60); len([]rune(got)) != 60 {
		t.Fatalf("trunc kept %d runes, want 60", len([]rune(got)))
	}
	if got := trunc("(int)a", 60); got != "(int)a" {
		t.Fatalf("trunc shortened a short argument: %q", got)
	}
	found := scan("TEST_ASSERT_EQ("+long+", b);\n", "a.c")
	if len(found) != 1 {
		t.Fatalf("findings = %+v, want one", found)
	}
	for _, r := range found[0] {
		if r == '\ufffd' {
			t.Fatalf("the cut split a rune: %q", found[0])
		}
	}
}

// An unterminated call still has to answer rather than run off the end of the
// file, and the argument before the comma is still judged.
func TestAnUnterminatedCallIsJudgedToTheEndOfTheFile(t *testing.T) {
	found := scan("TEST_ASSERT_EQ((int)a, b\n", "a.c")
	if len(found) != 1 {
		t.Fatalf("findings = %+v, want the first argument reported", found)
	}
	if !strings.Contains(found[0], "a.c:1: cast in first arg") {
		t.Fatalf("finding = %q", found[0])
	}
}
