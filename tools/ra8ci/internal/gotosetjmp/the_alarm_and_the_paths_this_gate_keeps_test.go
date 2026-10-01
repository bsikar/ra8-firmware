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
	"time"
)

// withSourceExtensions, withSourceRoots, withExcludedPrefixes and
// withBuildTreeRoots blind one of the tables the scope decision is made from,
// so the self-test is asked whether it notices its own detector going wrong.
func withSourceExtensions(t *testing.T, table map[string]struct{}) {
	t.Helper()
	shipped := sourceExtensions
	sourceExtensions = table
	t.Cleanup(func() { sourceExtensions = shipped })
}

func withSourceRoots(t *testing.T, table map[string]struct{}) {
	t.Helper()
	shipped := sourceRoots
	sourceRoots = table
	t.Cleanup(func() { sourceRoots = shipped })
}

func withExcludedPrefixes(t *testing.T, prefixes []string) {
	t.Helper()
	shipped := excludedPrefixes
	excludedPrefixes = prefixes
	t.Cleanup(func() { excludedPrefixes = shipped })
}

func withBuildTreeRoots(t *testing.T, table map[string]struct{}) {
	t.Helper()
	shipped := buildTreeRoots
	buildTreeRoots = table
	t.Cleanup(func() { buildTreeRoots = shipped })
}

func selfTested(t *testing.T) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestTheSelfTestPassesOnTheTablesThatShipped(t *testing.T) {
	code, stdout, stderr := selfTested(t)
	if code != 0 {
		t.Fatalf("shipped tables: exit %d, stderr %q", code, stderr)
	}
	if !strings.Contains(stdout, "all cases pass.") {
		t.Fatalf("shipped tables: stdout %q", stdout)
	}
	if strings.Contains(stdout, "[FAIL]") {
		t.Fatalf("shipped tables reported a failing case: %q", stdout)
	}
}

func TestASelfTestWithNoSourceExtensionsCountsEveryScopedCaseItLost(t *testing.T) {
	withSourceExtensions(t, map[string]struct{}{})
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("blinded extensions: exit %d, stderr %q", code, stderr)
	}
	// The three cases the gate is meant to claim are the three it now drops.
	for _, want := range []string{
		"[FAIL] scope tools/ra8ci/main.cpp",
		"[FAIL] scope examples/app/src/main.c",
		"[FAIL] scope tools/builders/build_app.c",
	} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("blinded extensions: stdout %q lacks %q", stdout, want)
		}
	}
	if !strings.Contains(stderr, "--selftest: 3 failure(s)") {
		t.Fatalf("blinded extensions: stderr %q", stderr)
	}
	if strings.Contains(stdout, "all cases pass.") {
		t.Fatalf("blinded extensions still announced a pass: %q", stdout)
	}
}

func TestASelfTestWithNoSourceRootsCountsEveryScopedCaseItLost(t *testing.T) {
	withSourceRoots(t, map[string]struct{}{})
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("blinded roots: exit %d, stderr %q", code, stderr)
	}
	if !strings.Contains(stderr, "--selftest: 3 failure(s)") {
		t.Fatalf("blinded roots: stderr %q", stderr)
	}
	if !strings.Contains(stdout, "[FAIL] scope tools/ra8ci/main.cpp") {
		t.Fatalf("blinded roots: stdout %q", stdout)
	}
}

func TestASelfTestWithNoExclusionsCountsTheVendoredTreesItAdmitted(t *testing.T) {
	withExcludedPrefixes(t, nil)
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("no exclusions: exit %d, stderr %q", code, stderr)
	}
	// Vendored and generated trees are the whole point of the prefix list.
	for _, want := range []string{
		"[FAIL] scope libs/third_party/vendor.c",
		"[FAIL] scope apps/shared_libs/third_party/vendor.cpp",
		"[FAIL] scope libs/ra8_fonts/table.c",
	} {
		if !strings.Contains(stdout, want) {
			t.Fatalf("no exclusions: stdout %q lacks %q", stdout, want)
		}
	}
	if !strings.Contains(stderr, "--selftest: 3 failure(s)") {
		t.Fatalf("no exclusions: stderr %q", stderr)
	}
}

func TestASelfTestWithNoBuildTreeRootsCountsTheBuildOutputItAdmitted(t *testing.T) {
	withBuildTreeRoots(t, map[string]struct{}{})
	code, stdout, stderr := selfTested(t)
	if code != 1 {
		t.Fatalf("no build roots: exit %d, stderr %q", code, stderr)
	}
	// A nested build directory is only output because its top level says so,
	// so emptying that table loses exactly this one case and no other.
	if !strings.Contains(stdout, "[FAIL] scope tools/project/build/out.c") {
		t.Fatalf("no build roots: stdout %q", stdout)
	}
	if !strings.Contains(stderr, "--selftest: 1 failure(s)") {
		t.Fatalf("no build roots: stderr %q", stderr)
	}
}

// aContextThatCancelsAfterItsFirstAnswer reports no cancellation the first time
// it is asked and a cancellation every time after, so the enumeration runs to
// completion and the scan that follows it is the step that is interrupted. Its
// Done channel is never closed, so the git child is never killed.
type aContextThatCancelsAfterItsFirstAnswer struct {
	never  chan struct{}
	answer *int
}

func (c aContextThatCancelsAfterItsFirstAnswer) Deadline() (time.Time, bool) {
	return time.Time{}, false
}

func (c aContextThatCancelsAfterItsFirstAnswer) Done() <-chan struct{} { return c.never }

func (c aContextThatCancelsAfterItsFirstAnswer) Value(any) any { return nil }

func (c aContextThatCancelsAfterItsFirstAnswer) Err() error {
	*c.answer++
	if *c.answer <= 1 {
		return nil
	}
	return context.Canceled
}

func TestACancellationPartWayThroughTheScanIsNotACleanTree(t *testing.T) {
	root := plantSourceRepo(t, map[string]string{
		"libs/a.c": "int a(void) { return 0; }\n",
		"libs/b.c": "int b(void) { return 0; }\n",
		"libs/c.c": "int c(void) { return 0; }\n",
	})
	answers := 0
	ctx := aContextThatCancelsAfterItsFirstAnswer{never: make(chan struct{}), answer: &answers}
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, nil, &stdout, &stderr)
	if code != 2 {
		t.Fatalf("cancelled scan: exit %d, stdout %q, stderr %q", code, stdout.String(), stderr.String())
	}
	if !strings.Contains(stderr.String(), "cancelled") {
		t.Fatalf("cancelled scan: stderr %q", stderr.String())
	}
	// A partial walk must not be announced as a scanned, clean tree.
	if strings.Contains(stdout.String(), "PASS") {
		t.Fatalf("cancelled scan announced a pass: %q", stdout.String())
	}
}

// plantConflictedRepo leaves one source file conflicted, so the index carries
// it at three stages and `git ls-files --cached` names it three times.
func plantConflictedRepo(t *testing.T) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable")
	}
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "libs"), 0o755); err != nil {
		t.Fatalf("plant: %v", err)
	}
	run := func(args ...string) string {
		t.Helper()
		command := exec.Command("git", append([]string{"-C", root,
			"-c", "user.name=Test", "-c", "user.email=test@example.invalid",
			"-c", "commit.gpgsign=false"}, args...)...)
		output, err := command.CombinedOutput()
		if err != nil && args[0] != "merge" {
			t.Fatalf("git %v: %v: %s", args, err, output)
		}
		return string(output)
	}
	write := func(body string) {
		t.Helper()
		if err := os.WriteFile(filepath.Join(root, "libs", "a.c"), []byte(body), 0o644); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
	run("init", "-q")
	write("int a(void) { return 0; }\n")
	run("add", "libs/a.c")
	run("commit", "-q", "-m", "base")
	base := strings.TrimSpace(run("rev-parse", "--abbrev-ref", "HEAD"))
	run("checkout", "-q", "-b", "side")
	write("int a(void) { return 1; }\n")
	run("commit", "-q", "-a", "-m", "side")
	run("checkout", "-q", base)
	write("int a(void) { return 2; }\n")
	run("commit", "-q", "-a", "-m", "ours")
	run("merge", "side")
	return root
}

func TestAFileConflictedAtThreeStagesIsScannedOnce(t *testing.T) {
	root := plantConflictedRepo(t)
	staged := 0
	for _, line := range strings.Split(string(mustListFiles(t, root)), "\x00") {
		if line == "libs/a.c" {
			staged++
		}
	}
	if staged < 2 {
		t.Skipf("this git does not enumerate conflict stages separately (%d entries)", staged)
	}
	code, stdout, stderr := ranGate(t, context.Background(), root)
	if code != 0 {
		t.Fatalf("conflicted tree: exit %d, stderr %q", code, stderr)
	}
	// One file on disk is one file scanned, however many index stages hold it.
	if !strings.Contains(stdout, "1 file(s) scanned") {
		t.Fatalf("conflicted tree: stdout %q, staged entries %d", stdout, staged)
	}
}

func mustListFiles(t *testing.T, root string) []byte {
	t.Helper()
	output, err := exec.Command("git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard").Output()
	if err != nil {
		t.Fatalf("ls-files: %v", err)
	}
	return output
}
