// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testsreadme

import (
	"bytes"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Run is the whole gate as CI invokes it: a root, a README, and three exit
// codes an operator has to be able to tell apart. These tests hold what each
// exit says and where it says it, so a collapsed scan is never read as a clean
// tree and drift is never read as a broken box.

// plantRoot builds a repository root holding tests/<subdirs> and, unless the
// README body is empty, tests/README.md with one table row per documented name.
func plantRoot(t *testing.T, documented []string, subdirs ...string) string {
	t.Helper()
	root := t.TempDir()
	testsDir := filepath.Join(root, "tests")
	if err := os.MkdirAll(testsDir, 0o755); err != nil {
		t.Fatal(err)
	}
	for _, name := range subdirs {
		if err := os.Mkdir(filepath.Join(testsDir, name), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if documented == nil {
		return root
	}
	var rows strings.Builder
	rows.WriteString("# tests\n\n| directory | what it holds |\n| --- | --- |\n")
	for _, name := range documented {
		rows.WriteString("| `" + name + "/` | fixture description |\n")
	}
	if err := os.WriteFile(filepath.Join(testsDir, "README.md"), []byte(rows.String()), 0o644); err != nil {
		t.Fatal(err)
	}
	return root
}

// checked is one invocation of Run over a planted root.
type checked struct {
	code   int
	stdout string
	stderr string
}

func check(t *testing.T, root string, args ...string) checked {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return checked{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

func fiveNames() []string {
	return []string{"alpha", "beta", "delta", "epsilon", "gamma"}
}

func TestATreeMatchingItsREADMEIsReportedCleanOnStdout(t *testing.T) {
	names := fiveNames()
	got := check(t, plantRoot(t, names, names...))
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("in-sync tree = %+v", got)
	}
	for _, want := range []string{"tests/README.md OK", "5 subdirectory(ies) documented", "none stale"} {
		if !strings.Contains(got.stdout, want) {
			t.Fatalf("clean report %q omits %q", got.stdout, want)
		}
	}
}

func TestAnUndocumentedSubdirectoryIsDriftAndNamesTheRowToAdd(t *testing.T) {
	names := fiveNames()
	got := check(t, plantRoot(t, fiveNames()[:4:4], names...))
	if got.code != 1 || got.stdout != "" {
		t.Fatalf("undocumented subdir = %+v", got)
	}
	for _, want := range []string{"tests/README.md drift: 1 problem(s):", "tests/gamma/ exists but is not documented", "add a table row whose first cell is `gamma/`"} {
		if !strings.Contains(got.stderr, want) {
			t.Fatalf("drift report %q omits %q", got.stderr, want)
		}
	}
}

func TestAStaleREADMERowIsDriftAndSaysToRemoveIt(t *testing.T) {
	names := fiveNames()
	got := check(t, plantRoot(t, append(names, "ghost"), names...))
	if got.code != 1 || got.stdout != "" {
		t.Fatalf("stale row = %+v", got)
	}
	if !strings.Contains(got.stderr, "documents tests/ghost/ but no such subdirectory exists") || !strings.Contains(got.stderr, "remove or rename that row") {
		t.Fatalf("stale report = %q", got.stderr)
	}
}

// Both directions at once must be counted together: an operator fixing one
// problem needs to know the other is still there.
func TestBothDriftDirectionsAreCountedInOneReport(t *testing.T) {
	names := fiveNames()
	documented := append(fiveNames()[:4:4], "ghost")
	got := check(t, plantRoot(t, documented, names...))
	if got.code != 1 || !strings.Contains(got.stderr, "drift: 2 problem(s):") {
		t.Fatalf("two-way drift = %+v", got)
	}
	exists := strings.Index(got.stderr, "tests/gamma/ exists")
	documents := strings.Index(got.stderr, "documents tests/ghost/")
	if exists < 0 || documents < 0 || exists > documents {
		t.Fatalf("the undocumented half should be reported first: %q", got.stderr)
	}
}

// The floor is the whole point of the gate: a tree that lost its subdirectories
// must not be reported as a README with nothing stale in it.
func TestATreeUnderTheFloorIsACollapsedScanNotACleanTree(t *testing.T) {
	names := fiveNames()[:4]
	got := check(t, plantRoot(t, names, names...))
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("under the floor = %+v", got)
	}
	for _, want := range []string{"collapsed scan: 1 problem(s):", "only 4 subdirectory(ies) found", "floor is 5", "the scan collapsed rather than the tree"} {
		if !strings.Contains(got.stderr, want) {
			t.Fatalf("floor report %q omits %q", got.stderr, want)
		}
	}
	if strings.Contains(got.stderr, "drift") {
		t.Fatalf("a collapsed scan must not be labelled drift: %q", got.stderr)
	}
}

func TestAnEmptyTestsDirectoryIsStillACollapsedScan(t *testing.T) {
	got := check(t, plantRoot(t, []string{}))
	if got.code != 2 || !strings.Contains(got.stderr, "only 0 subdirectory(ies) found") {
		t.Fatalf("empty tests directory = %+v", got)
	}
}

func TestARootWithNoTestsDirectoryIsARefusalNotAReport(t *testing.T) {
	got := check(t, t.TempDir())
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("absent tests directory = %+v", got)
	}
	if !strings.Contains(got.stderr, "tests-readme:") || !strings.Contains(got.stderr, "read ") {
		t.Fatalf("refusal = %q", got.stderr)
	}
	if strings.Contains(got.stderr, "problem(s)") {
		t.Fatalf("a missing tree is not a problem count: %q", got.stderr)
	}
}

// An absent README is drift against every subdirectory, not a read failure:
// os.ErrNotExist is deliberately passed over so the gate can say what to write.
func TestAnAbsentREADMEIsDriftAgainstEverySubdirectory(t *testing.T) {
	names := fiveNames()
	got := check(t, plantRoot(t, nil, names...))
	if got.code != 1 || !strings.Contains(got.stderr, "drift: 5 problem(s):") {
		t.Fatalf("absent README = %+v", got)
	}
	for _, name := range names {
		if !strings.Contains(got.stderr, "tests/"+name+"/ exists but is not documented") {
			t.Fatalf("absent-README report omits %s: %q", name, got.stderr)
		}
	}
}

func TestAnUnreadableREADMEIsARefusalRatherThanDrift(t *testing.T) {
	names := fiveNames()
	root := plantRoot(t, names, names...)
	readme := filepath.Join(root, "tests", "README.md")
	if err := os.Chmod(readme, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(readme, 0o644) })
	if os.Geteuid() == 0 {
		t.Skip("root reads a mode 0o000 file regardless of its mode")
	}
	got := check(t, root)
	if got.code != 2 || !strings.Contains(got.stderr, "read ") || strings.Contains(got.stderr, "problem(s)") {
		t.Fatalf("unreadable README = %+v", got)
	}
}

// Only immediate directories count, and a dotted name is never one of them.
func TestFilesAndDottedNamesAreNotSubdirectories(t *testing.T) {
	names := fiveNames()
	root := plantRoot(t, names, names...)
	testsDir := filepath.Join(root, "tests")
	if err := os.Mkdir(filepath.Join(testsDir, ".hidden"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(testsDir, "loose.md"), []byte("not a directory\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(testsDir, "alpha", "nested"), 0o755); err != nil {
		t.Fatal(err)
	}
	got := check(t, root)
	if got.code != 0 || !strings.Contains(got.stdout, "5 subdirectory(ies)") {
		t.Fatalf("dotted, loose and nested names should not be counted: %+v", got)
	}
}

// The carve-out that keeps a developer's own scratch directory out of the
// gate: a subdirectory Git ignores is neither counted nor demanded.
func TestAnIgnoredSubdirectoryIsNeitherCountedNorDemanded(t *testing.T) {
	if _, err := os.Stat(trustedGit); err != nil {
		t.Skipf("trusted git is unavailable: %v", err)
	}
	names := fiveNames()
	root := plantRoot(t, names, append(names, "scratch")...)
	if err := os.WriteFile(filepath.Join(root, ".gitignore"), []byte("tests/scratch/\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command(trustedGit, "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git init unavailable: %v: %s", err, output)
	}
	got := check(t, root)
	if got.code != 0 || !strings.Contains(got.stdout, "5 subdirectory(ies)") {
		t.Fatalf("an ignored subdirectory reached the gate: %+v", got)
	}
}

func TestTheSelfTestProvesBothDriftDirectionsAndTheFloor(t *testing.T) {
	got := check(t, t.TempDir(), "--selftest")
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("selftest = %+v", got)
	}
	for _, want := range []string{"selftest OK:", "4 cases", "gitignore carve-out", "both drift directions", "non-vacuity floor"} {
		if !strings.Contains(got.stdout, want) {
			t.Fatalf("selftest report %q omits %q", got.stdout, want)
		}
	}
}

// The self-test builds its own fixtures, so it must not care what root it was
// handed: a root with no tests tree at all still answers 0.
func TestTheSelfTestIgnoresTheRootItWasHanded(t *testing.T) {
	got := check(t, filepath.Join(t.TempDir(), "no-such-tree"), "--selftest")
	if got.code != 0 || !strings.Contains(got.stdout, "selftest OK:") {
		t.Fatalf("selftest over an absent root = %+v", got)
	}
}

func TestACancelledSelfTestFailsRatherThanPassingVacuously(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	if code := Run(ctx, t.TempDir(), []string{"--selftest"}, &stdout, &stderr); code != 1 {
		t.Fatalf("cancelled selftest = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stderr.String(), "selftest FAILED") || !strings.Contains(stderr.String(), "cancelled") {
		t.Fatalf("cancelled selftest stderr = %q", stderr.String())
	}
	if strings.Contains(stdout.String(), "selftest OK") {
		t.Fatalf("a cancelled selftest announced success: %q", stdout.String())
	}
}

func TestACancelledCheckIsRefusedRatherThanReported(t *testing.T) {
	names := fiveNames()
	root := plantRoot(t, names, names...)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, nil, &stdout, &stderr)
	if code != 2 || stdout.String() != "" || !strings.Contains(stderr.String(), "tests-readme:") {
		t.Fatalf("cancelled check = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestAnIncompleteInvocationIsRefusedWithoutACrash(t *testing.T) {
	var stdout, stderr bytes.Buffer
	var absent io.Writer
	for _, test := range []struct {
		name   string
		ctx    context.Context
		root   string
		stdout io.Writer
		stderr io.Writer
	}{
		{"no context", nil, t.TempDir(), &stdout, &stderr},
		{"no root", context.Background(), "", &stdout, &stderr},
		{"no stdout", context.Background(), t.TempDir(), absent, &stderr},
		{"no stderr", context.Background(), t.TempDir(), &stdout, absent},
	} {
		stdout.Reset()
		stderr.Reset()
		code := Run(test.ctx, test.root, []string{"--selftest"}, test.stdout, test.stderr)
		if code != 2 {
			t.Fatalf("%s = %d", test.name, code)
		}
		if stdout.Len() != 0 {
			t.Fatalf("%s wrote %q to stdout", test.name, stdout.String())
		}
		if test.stderr != nil && test.name != "no stderr" && !strings.Contains(stderr.String(), "invalid input") {
			t.Fatalf("%s stderr = %q", test.name, stderr.String())
		}
	}
}
