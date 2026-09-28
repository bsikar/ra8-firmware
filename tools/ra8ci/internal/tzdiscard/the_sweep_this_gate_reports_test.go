// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// These tests hold the run itself: what an operator is told for a clean tree,
// for a violation, and for a sweep that collapsed. The detector's own rules are
// held next door; what is pinned here is the reporting and the scope decisions
// Run takes around it, including the floor that stops a sweep of nothing from
// being read as a clean tree.

const boundaryDiscard = "void app_main(void)\n{\n  (void)ra8_tz_secure_boot_verify();\n}\n"

// plantSource writes one file under root, creating its parents.
func plantSource(t *testing.T, root, rel, body string) string {
	t.Helper()
	full := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return full
}

// swept is one invocation of Run.
type swept struct {
	code   int
	stdout string
	stderr string
}

func sweep(t *testing.T, root string, args ...string) swept {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return swept{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

// plantTreeAboveTheFloor writes enough empty first-party sources to clear the
// file floor. An empty body holds no (void) cast, so the sweep stays clean and
// the cost is one inode per file rather than any scanning work.
func plantTreeAboveTheFloor(t *testing.T, root string, extra int) {
	t.Helper()
	dir := filepath.Join(root, "libs", "generated")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < fileFloor+extra; i++ {
		name := filepath.Join(dir, "unit"+strconv.Itoa(i)+".c")
		if err := os.WriteFile(name, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func TestAFileWithNothingDiscardedIsReportedClean(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", "void app_main(void)\n{\n  RA8_ERROR_CHECK(ra8_tz_secure_boot_verify());\n}\n")
	got := sweep(t, root, "libs/tz/boot.c")
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("clean file = %+v", got)
	}
	if !strings.Contains(got.stdout, "clean -- no silent ra8_err_t discards") {
		t.Fatalf("clean report = %q", got.stdout)
	}
}

func TestADiscardedWorldSwitchIsReportedWithItsLineAndRule(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	got := sweep(t, root, "libs/tz/boot.c")
	if got.code != 1 || got.stderr != "" {
		t.Fatalf("discarded world switch = %+v", got)
	}
	for _, want := range []string{
		"libs/tz/boot.c:3: [rule A]",
		"world-switch result discarded",
		"RA8_ERROR_CHECK",
		"(void)ra8_tz_secure_boot_verify();",
		"1 violation(s)",
		"ISO C23",
		"TZ-DISCARD-OK:",
	} {
		if !strings.Contains(got.stdout, want) {
			t.Fatalf("violation report %q omits %q", got.stdout, want)
		}
	}
}

// Rule B is the wider one and only applies inside a translation unit that
// actually defines a boot entry point, so the same call in a plain file is not
// a finding. Both halves have to hold or the gate is either blind or noisy.
func TestTheWiderRuleAppliesOnlyInsideABootTranslationUnit(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boots.c", "void SystemInit(void)\n{\n  (void)ra8_cgc_init();\n}\n")
	plantSource(t, root, "libs/tz/plain.c", "void helper(int flag)\n{\n  (void)ra8_cgc_init();\n}\n")
	boot := sweep(t, root, "libs/tz/boots.c")
	if boot.code != 1 || !strings.Contains(boot.stdout, "[rule B]") || !strings.Contains(boot.stdout, "boot-TU ra8_* result discarded") {
		t.Fatalf("boot translation unit = %+v", boot)
	}
	plain := sweep(t, root, "libs/tz/plain.c")
	if plain.code != 0 || !strings.Contains(plain.stdout, "clean --") {
		t.Fatalf("non-boot translation unit = %+v", plain)
	}
}

func TestEveryViolationIsCountedInTheClosingTotal(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/first.c", boundaryDiscard)
	plantSource(t, root, "libs/tz/second.c", boundaryDiscard)
	got := sweep(t, root, "libs/tz/second.c", "libs/tz/first.c")
	if got.code != 1 || !strings.Contains(got.stdout, "2 violation(s)") {
		t.Fatalf("two files = %+v", got)
	}
	first := strings.Index(got.stdout, "first.c")
	second := strings.Index(got.stdout, "second.c")
	if first < 0 || second < 0 || first > second {
		t.Fatalf("the report should be sorted by path: %q", got.stdout)
	}
}

// A file named twice on one command line is one file: a build system that
// passes a path through two rules must not double every violation under it.
func TestAFileNamedTwiceIsScannedOnce(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	got := sweep(t, root, "libs/tz/boot.c", "libs/tz/boot.c", "libs/tz/boot.c")
	if got.code != 1 || !strings.Contains(got.stdout, "1 violation(s)") {
		t.Fatalf("repeated path = %+v", got)
	}
}

// An explicit path is still judged by policy: naming a file does not buy it
// past the extension, build-output and vendored rules.
func TestAnExplicitPathOutsideThePolicyIsPassedOver(t *testing.T) {
	root := t.TempDir()
	for _, rel := range []string{
		"libs/tz/notes.md",
		"libs/tz/boot.py",
		"tests/build/boot.c",
		"libs/CMakeFiles/boot.c",
		"libs/third_party/vendor/boot.c",
		"libs/ra8_fonts/glyphs.c",
	} {
		plantSource(t, root, rel, boundaryDiscard)
		got := sweep(t, root, rel)
		if got.code != 0 || !strings.Contains(got.stdout, "clean --") {
			t.Fatalf("%s = %+v", rel, got)
		}
	}
}

func TestAnAbsolutePathIsJudgedRelativeToTheRoot(t *testing.T) {
	root := t.TempDir()
	watched := plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	got := sweep(t, root, watched)
	if got.code != 1 || !strings.Contains(got.stdout, "[rule A]") {
		t.Fatalf("absolute watched path = %+v", got)
	}
	vendored := plantSource(t, root, "libs/third_party/vendor/boot.c", boundaryDiscard)
	passed := sweep(t, root, vendored)
	if passed.code != 0 || !strings.Contains(passed.stdout, "clean --") {
		t.Fatalf("absolute vendored path = %+v", passed)
	}
}

// The floor is the whole point of the whole-tree sweep: a tree that lost its
// sources must be FATAL, never a clean report.
func TestACollapsedSweepIsFatalRatherThanClean(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	got := sweep(t, root)
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("collapsed sweep = %+v", got)
	}
	for _, want := range []string{"FATAL", "only 1 first-party source file(s) in scope", "floor is 1700", "A collapsed sweep reports a clean tree because it scanned nothing"} {
		if !strings.Contains(got.stderr, want) {
			t.Fatalf("floor refusal %q omits %q", got.stderr, want)
		}
	}
}

func TestAnEmptyTreeIsAlsoFatal(t *testing.T) {
	got := sweep(t, t.TempDir())
	if got.code != 2 || !strings.Contains(got.stderr, "only 0 first-party source file(s) in scope") {
		t.Fatalf("empty tree = %+v", got)
	}
}

func TestATreeAboveTheFloorIsSweptAndReported(t *testing.T) {
	root := t.TempDir()
	plantTreeAboveTheFloor(t, root, 0)
	clean := sweep(t, root)
	if clean.code != 0 || clean.stderr != "" || !strings.Contains(clean.stdout, "clean --") {
		t.Fatalf("swept tree = %+v", clean)
	}
	plantSource(t, root, "port/threadx/boot.c", boundaryDiscard)
	found := sweep(t, root)
	if found.code != 1 || !strings.Contains(found.stdout, "port/threadx/boot.c:3: [rule A]") || !strings.Contains(found.stdout, "1 violation(s)") {
		t.Fatalf("swept tree with a violation = %+v", found)
	}
}

// Discovery walks six declared roots. A directory it cannot read is a refusal,
// not a smaller scope that happens to pass the floor.
func TestADirectoryDiscoveryCannotReadIsARefusal(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root reads a mode 0o000 directory regardless of its mode")
	}
	root := t.TempDir()
	plantTreeAboveTheFloor(t, root, 0)
	sealed := filepath.Join(root, "libs", "sealed")
	if err := os.Mkdir(sealed, 0o000); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(sealed, 0o755) })
	got := sweep(t, root)
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "source discovery failed") {
		t.Fatalf("sealed directory = %+v", got)
	}
}

func TestACancelledSweepIsRefusedRatherThanReportedClean(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, []string{"libs/tz/boot.c"}, &stdout, &stderr)
	if code != 2 || stdout.String() != "" || !strings.Contains(stderr.String(), "cancelled") {
		t.Fatalf("cancelled sweep = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

func TestACancelledDiscoveryIsRefusedRatherThanReportedClean(t *testing.T) {
	root := t.TempDir()
	plantTreeAboveTheFloor(t, root, 0)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, nil, &stdout, &stderr)
	if code != 2 || stdout.String() != "" || !strings.Contains(stderr.String(), "source discovery failed") {
		t.Fatalf("cancelled discovery = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

// --selftest is the whole invocation or it is not honoured: anything else
// beginning with a dash is refused with the usage line, so a mistyped option
// never runs a sweep the caller did not ask for.
func TestTheSelfTestIsTheWholeInvocationOrNothing(t *testing.T) {
	root := t.TempDir()
	plantSource(t, root, "libs/tz/boot.c", boundaryDiscard)
	alone := sweep(t, root, "--selftest")
	if alone.code != 0 || !strings.Contains(alone.stdout, "selftest") {
		t.Fatalf("selftest alone = %+v", alone)
	}
	for _, args := range [][]string{
		{"--selftest", "libs/tz/boot.c"},
		{"libs/tz/boot.c", "--selftest"},
		{"--selftest", "--selftest"},
		{"-selftest"},
		{"--unknown"},
		{"-"},
	} {
		got := sweep(t, root, args...)
		if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "usage: ra8ci tz-boundary-discard") {
			t.Fatalf("%q = %+v", args, got)
		}
	}
}

// A file that is named but absent is not a finding and not a failure: the
// build system decides what exists, and a vanished generated source must not
// fail the gate.
func TestAnAbsentNamedFileIsNeitherAFindingNorAFailure(t *testing.T) {
	root := t.TempDir()
	got := sweep(t, root, "libs/tz/vanished.c")
	if got.code != 0 || !strings.Contains(got.stdout, "clean --") {
		t.Fatalf("absent named file = %+v", got)
	}
}
