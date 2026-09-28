// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package sincegate

import (
	"bytes"
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// The self-test is what CI trusts when it has no findings to look at: it
// proves the detector both fires and stays quiet, and that the derived scope
// still reaches first-party code without reaching vendored code. Every way it
// can fail has to say WHICH half failed, or a green gate means nothing.

// plantRepo builds a repository root with a VERSION file and enough tracked
// paths to clear the floor. Empty files are enough: the floor counts regular
// paths, and `git ls-files --others --exclude-standard` answers untracked
// files, so no commit and no git identity are needed.
func plantRepo(t *testing.T, version string, extra map[string]string) string {
	t.Helper()
	root := t.TempDir()
	if version != "" {
		if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte(version+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	filler := filepath.Join(root, "docs", "filler")
	if err := os.MkdirAll(filler, 0o755); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < trackedFloor; i++ {
		if err := os.WriteFile(filepath.Join(filler, "note"+strconv.Itoa(i)+".md"), nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for rel, body := range extra {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	if output, err := exec.Command("git", "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git is unavailable: %v: %s", err, output)
	}
	return root
}

// firstParty is the one file that proves the derived scope still reaches our
// own code: the self-test refuses a scope with no tools/ path in it.
var firstParty = map[string]string{"tools/ra8ci/probe.c": "/** @since 1.4.0 */\n"}

// gated is one invocation of Run.
type gated struct {
	code   int
	stdout string
	stderr string
}

func gate(t *testing.T, root string, args ...string) gated {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return gated{code: code, stdout: stdout.String(), stderr: stderr.String()}
}

func TestTheSelfTestPassesOverAScopeThatReachesOurOwnCode(t *testing.T) {
	got := gate(t, plantRepo(t, "1.4.0", firstParty), "--selftest")
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("selftest = %+v", got)
	}
	for _, want := range []string{"selftest passed", "wrong/right values", "missing API tag", "derived scope"} {
		if !strings.Contains(got.stdout, want) {
			t.Fatalf("selftest report %q omits %q", got.stdout, want)
		}
	}
}

// The self-test writes its fixtures against the project's OWN version, so a
// VERSION file it cannot read or trust must fail it rather than let it pass
// against a version it invented.
func TestASelfTestWithNoTrustworthyVersionFails(t *testing.T) {
	absent := gate(t, plantRepo(t, "", firstParty), "--selftest")
	if absent.code != 2 || absent.stdout != "" || !strings.Contains(absent.stderr, "VERSION") {
		t.Fatalf("absent VERSION = %+v", absent)
	}
	for _, bad := range []string{"1.4", "v1.4.0", "1.4.0-rc1", "not-a-version", ""} {
		root := plantRepo(t, "1.4.0", firstParty)
		if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte(bad+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		got := gate(t, root, "--selftest")
		if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "is not semver MAJOR.MINOR.PATCH") {
			t.Fatalf("VERSION %q = %+v", bad, got)
		}
	}
}

// A scope that collapsed is the failure this floor exists for: without it a
// self-test over a tree with nothing in it would announce success.
func TestASelfTestOverACollapsedScopeFails(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte("1.4.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if output, err := exec.Command("git", "init", "-q", root).CombinedOutput(); err != nil {
		t.Skipf("git is unavailable: %v: %s", err, output)
	}
	got := gate(t, root, "--selftest")
	if got.code != 2 || got.stdout != "" {
		t.Fatalf("collapsed scope = %+v", got)
	}
	if !strings.Contains(got.stderr, "tracked path(s), floor is 1000") {
		t.Fatalf("floor refusal = %q", got.stderr)
	}
}

func TestASelfTestOutsideARepositoryFails(t *testing.T) {
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "VERSION"), []byte("1.4.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	got := gate(t, root, "--selftest")
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "git ls-files failed") {
		t.Fatalf("non-repository = %+v", got)
	}
}

// Both halves of the scope assertion, each naming what it saw: a scope that
// reaches no first-party code, and one that reaches vendored code it should
// have excluded.
func TestASelfTestNamesWhichHalfOfTheScopeFailed(t *testing.T) {
	blind := gate(t, plantRepo(t, "1.4.0", nil), "--selftest")
	if blind.code != 2 || blind.stdout != "" || !strings.Contains(blind.stderr, "scope selftest: tools=false vendored=false") {
		t.Fatalf("scope with no first-party code = %+v", blind)
	}
}

func TestTheVersionMustBeReadableBeforeAnyFileIsScanned(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{"libs/tz/boot.c": "/** @since 9.9.9 */\n"})
	if err := os.Remove(filepath.Join(root, "VERSION")); err != nil {
		t.Fatal(err)
	}
	got := gate(t, root, filepath.Join(root, "libs", "tz", "boot.c"))
	if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "VERSION") {
		t.Fatalf("absent VERSION = %+v", got)
	}
	if strings.Contains(got.stderr, "issue(s) found") {
		t.Fatalf("a missing version must not be reported as findings: %q", got.stderr)
	}
}

func TestAStaleSinceValueIsReportedAgainstTheProjectVersion(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{"libs/tz/boot.c": "/** @since 9.9.9 */\nra8_err_t ra8_boot(void);\n"})
	got := gate(t, root, filepath.Join(root, "libs", "tz", "boot.c"))
	if got.code != 1 || got.stdout != "" {
		t.Fatalf("stale @since = %+v", got)
	}
	for _, want := range []string{"project version is 1.4.0", "boot.c:1: @since 9.9.9 != project 1.4.0", "1 issue(s) found."} {
		if !strings.Contains(got.stderr, want) {
			t.Fatalf("report %q omits %q", got.stderr, want)
		}
	}
}

// Presence is only demanded of a public header under libs/ra8_*/inc/: the same
// declaration anywhere else is not the public API surface.
func TestAMissingTagIsDemandedOnlyOfAPublicHeader(t *testing.T) {
	bare := "ra8_err_t ra8_public(void);\n"
	root := plantRepo(t, "1.4.0", map[string]string{
		"libs/ra8_tz/inc/ra8_tz.h": bare,
		"libs/ra8_tz/src/ra8_tz.h": bare,
		"tools/scratch/inc/ra8.h":  bare,
	})
	public := gate(t, root, filepath.Join(root, "libs", "ra8_tz", "inc", "ra8_tz.h"))
	if public.code != 1 || !strings.Contains(public.stderr, "ra8_public missing @since") || !strings.Contains(public.stderr, "1 issue(s) found.") {
		t.Fatalf("public header = %+v", public)
	}
	for _, rel := range []string{"libs/ra8_tz/src/ra8_tz.h", "tools/scratch/inc/ra8.h"} {
		got := gate(t, root, filepath.Join(root, filepath.FromSlash(rel)))
		if got.code != 0 || got.stderr != "" {
			t.Fatalf("%s = %+v", rel, got)
		}
	}
}

func TestEveryFindingIsCountedInOneClosingTotal(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{
		"libs/ra8_tz/inc/ra8_tz.h": "ra8_err_t ra8_first(void);\nra8_err_t ra8_second(void);\n",
		"libs/tz/boot.c":           "/** @since 0.1.0 */\n/** @since 2.0.0 */\n",
	})
	got := gate(t, root, filepath.Join(root, "libs", "ra8_tz", "inc", "ra8_tz.h"), filepath.Join(root, "libs", "tz", "boot.c"))
	if got.code != 1 || !strings.Contains(got.stderr, "4 issue(s) found.") {
		t.Fatalf("four findings = %+v", got)
	}
}

// A named path that is absent or is not a regular file is passed over: the
// build system decides what exists, and a directory is not a source file.
func TestAnAbsentOrIrregularPathIsPassedOver(t *testing.T) {
	root := plantRepo(t, "1.4.0", nil)
	for _, name := range []string{
		filepath.Join(root, "libs", "vanished.c"),
		filepath.Join(root, "docs"),
	} {
		got := gate(t, root, name)
		if got.code != 0 || got.stderr != "" {
			t.Fatalf("%s = %+v", name, got)
		}
	}
}

// A non-source extension carries no @since policy at all, even when it holds a
// stale-looking tag.
func TestANonSourceFileIsNotJudged(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{"docs/notes.md": "/** @since 9.9.9 */\n"})
	got := gate(t, root, filepath.Join(root, "docs", "notes.md"))
	if got.code != 0 || got.stderr != "" {
		t.Fatalf("markdown = %+v", got)
	}
}

func TestTheDerivedSweepJudgesFirstPartySourceAndSkipsVendored(t *testing.T) {
	stale := "/** @since 9.9.9 */\n"
	root := plantRepo(t, "1.4.0", map[string]string{
		"tools/ra8ci/main.go":                  "package main\n",
		"libs/ra8_tz/src/boot.c":               stale,
		"libs/third_party/vendor/vendor.c":     stale,
		"apps/shared_libs/third_party/other.c": stale,
		"libs/ra8_fonts/glyphs.c":              stale,
		"tools/vela/generated/model.c":         stale,
		"port/threadx/tx_port.c":               stale,
		"tests/build/generated.c":              stale,
		"libs/ra8_tz/CMakeFiles/scratch.c":     stale,
	})
	got := gate(t, root, "--all")
	if got.code != 1 {
		t.Fatalf("derived sweep = %+v", got)
	}
	if !strings.Contains(got.stderr, "1 issue(s) found.") || !strings.Contains(got.stderr, filepath.Join("libs", "ra8_tz", "src", "boot.c")+":1:") {
		t.Fatalf("the sweep should report exactly the first-party source: %q", got.stderr)
	}
	for _, skipped := range []string{"third_party", "ra8_fonts", "generated", "threadx", "build", "CMakeFiles"} {
		if strings.Contains(got.stderr, skipped) {
			t.Fatalf("%s reached the sweep: %q", skipped, got.stderr)
		}
	}
}

func TestADerivedSweepOverACleanTreeIsSilent(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{
		"tools/ra8ci/probe.c":    "/** @since 1.4.0 */\n",
		"libs/ra8_tz/src/boot.c": "/** @since 1.4.0 */\n",
	})
	got := gate(t, root, "--all")
	if got.code != 0 || got.stdout != "" || got.stderr != "" {
		t.Fatalf("clean sweep = %+v", got)
	}
}

func TestACancelledScanIsRefusedRatherThanReportedClean(t *testing.T) {
	root := plantRepo(t, "1.4.0", map[string]string{"libs/tz/boot.c": "/** @since 9.9.9 */\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	code := Run(ctx, root, []string{filepath.Join(root, "libs", "tz", "boot.c")}, &stdout, &stderr)
	if code != 2 || stdout.String() != "" || !strings.Contains(stderr.String(), "scan cancelled") {
		t.Fatalf("cancelled scan = %d, stdout=%q stderr=%q", code, stdout.String(), stderr.String())
	}
}

// An option the gate does not know is refused with the usage line rather than
// treated as a path, so a mistyped flag never scans nothing and passes.
func TestAnUnknownOptionIsRefusedWithTheUsageLine(t *testing.T) {
	root := plantRepo(t, "1.4.0", firstParty)
	for _, args := range [][]string{
		{"--unknown"},
		{"--all", "--selftest"},
		{"--selftest", "extra"},
		{"--all", "libs/tz/boot.c"},
		{"-"},
		{},
	} {
		got := gate(t, root, args...)
		if got.code != 2 || got.stdout != "" || !strings.Contains(got.stderr, "usage: ra8ci since FILE") {
			t.Fatalf("%q = %+v", args, got)
		}
	}
}
