// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"syscall"
	"testing"
)

func derivedScope(t *testing.T, root string) []string {
	t.Helper()
	paths, err := scopedFiles(context.Background(), root)
	if err != nil {
		t.Fatalf("scopedFiles: %v", err)
	}
	return paths
}

func holds(paths []string, target string) bool {
	for _, path := range paths {
		if path == target {
			return true
		}
	}
	return false
}

func TestScopeIsSortedDeduplicatedAndFirstParty(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"README.md":          "prose\n",
		"infra/deploy.sh":    "#!/bin/sh\n",
		"apps/ui/main.c":     "int main(void){return 0;}\n",
		"justfile":           "build:\n",
		"docs/notes.txt":     "text\n",
		gateSource:           "package unsafeinstall\n",
		"tools/ra8ci/go.mod": "module x\n",
	})
	got := derivedScope(t, root)
	want := []string{
		"README.md",
		"apps/ui/main.c",
		"docs/notes.txt",
		"infra/deploy.sh",
		"justfile",
		"tools/ra8ci/go.mod",
		gateSource,
	}
	// The gate sorts and de-duplicates, and it adds its own source to a set it
	// may already hold, so the answer carries exactly one copy of it.
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeDropsEveryExcludedPrefix(t *testing.T) {
	excluded := []string{
		"docs/sbom/upstream/pkg.json",
		"libs/third_party/vendor.c",
		"apps/shared_libs/third_party/vendor.c",
		"port/netxduo/nx.c",
		"port/nimble/ble.c",
		"port/threadx/tx.c",
		"port/usbx/ux.c",
		"tests/fixtures/sample.txt",
	}
	// Each of these shares a leading run of characters with an excluded prefix
	// without being under it, so they pin that the prefix is not a loose
	// substring and is anchored at the start of the path.
	kept := []string{
		"docs/sbom/inventory.json",
		"libs/third_partytools/a.c",
		"apps/shared_libs/first_party/a.c",
		"port/netxduo_notes.md",
		"apps/port/threadx/tx.c",
		"tests/fixtures.md",
		"tests/fixtures_extra/sample.txt",
	}
	files := map[string]string{gateSource: "package unsafeinstall\n"}
	for _, rel := range append(append([]string{}, excluded...), kept...) {
		files[rel] = "text\n"
	}
	got := derivedScope(t, plantRepo(t, files))
	for _, rel := range excluded {
		if holds(got, rel) {
			t.Errorf("scope holds excluded %s", rel)
		}
		if !isExcluded(rel) {
			t.Errorf("isExcluded(%q) = false", rel)
		}
	}
	for _, rel := range kept {
		if !holds(got, rel) {
			t.Errorf("scope drops first-party %s", rel)
		}
		if isExcluded(rel) {
			t.Errorf("isExcluded(%q) = true", rel)
		}
	}
}

func TestScopeCarriesTheGateSourceGitWouldHide(t *testing.T) {
	// The gate's own source is what the scope floor is checked against, so it is
	// added by name after the enumeration. An ignore rule over it does not
	// remove it; that is what keeps a collapsed scope from reading as clean.
	root := plantRepo(t, map[string]string{
		".gitignore": "tools/\n",
		"README.md":  "prose\n",
		gateSource:   "package unsafeinstall\n",
	})
	got := derivedScope(t, root)
	if !holds(got, gateSource) {
		t.Fatalf("scope = %v, want it to carry %s", got, gateSource)
	}
	if !hasPath(got, gateSource) {
		t.Fatalf("hasPath(scope, gate source) = false")
	}
}

func TestScopeDoesNotInventAnAbsentGateSource(t *testing.T) {
	got := derivedScope(t, plantRepo(t, map[string]string{"README.md": "prose\n"}))
	if holds(got, gateSource) {
		t.Fatalf("scope = %v, want no %s", got, gateSource)
	}
	if hasPath(got, gateSource) {
		t.Fatalf("hasPath(scope, absent gate source) = true")
	}
}

func TestScopeHonoursGitignoreAndStillTakesUntrackedFiles(t *testing.T) {
	root := plantRepo(t, map[string]string{
		".gitignore":      "ignored.txt\nbuild/\n",
		"ignored.txt":     "hidden\n",
		"build/out.txt":   "hidden\n",
		"untracked.md":    "prose\n",
		"docs/nested.txt": "prose\n",
	})
	got := derivedScope(t, root)
	for _, rel := range []string{"ignored.txt", "build/out.txt"} {
		if holds(got, rel) {
			t.Errorf("scope holds ignored %s", rel)
		}
	}
	// Nothing is committed in this fixture, so every kept path arrives through
	// --others: an uncommitted tree is still scanned.
	for _, rel := range []string{".gitignore", "untracked.md", "docs/nested.txt"} {
		if !holds(got, rel) {
			t.Errorf("scope drops %s", rel)
		}
	}
}

func TestScopeTakesOnlyWhatStatsAsARegularFile(t *testing.T) {
	root := plantRepo(t, map[string]string{"real.txt": "prose\n"})
	if err := os.Symlink(filepath.Join(root, "real.txt"), filepath.Join(root, "alias.txt")); err != nil {
		t.Skipf("symlink: %v", err)
	}
	if err := os.Symlink(filepath.Join(root, "gone.txt"), filepath.Join(root, "broken.txt")); err != nil {
		t.Skipf("symlink: %v", err)
	}
	if err := syscall.Mkfifo(filepath.Join(root, "pipe"), 0o644); err != nil {
		t.Skipf("mkfifo: %v", err)
	}
	got := derivedScope(t, root)
	// os.Stat follows the link, so a symlink to a readable regular file is read
	// as that file. A broken link stats to an error and a pipe is not regular;
	// both are passed over rather than failing the enumeration.
	if !holds(got, "alias.txt") {
		t.Errorf("scope drops a symlink to a regular file: %v", got)
	}
	for _, rel := range []string{"broken.txt", "pipe"} {
		if holds(got, rel) {
			t.Errorf("scope holds %s", rel)
		}
	}
}

func TestScopeRefusesADirectoryThatIsNotARepository(t *testing.T) {
	paths, err := scopedFiles(context.Background(), plantTree(t, map[string]string{"README.md": "prose\n"}))
	if err == nil {
		t.Fatalf("scopedFiles on a non-repository = %v, want an error", paths)
	}
	if paths != nil {
		t.Fatalf("scopedFiles paths = %v, want nil", paths)
	}
	// An empty scope would be read as a clean tree, so the failure has to be
	// named rather than answered with no paths.
	if !strings.Contains(err.Error(), "git ls-files") {
		t.Fatalf("error = %q, want it to name git ls-files", err)
	}
}

func TestHasPathMatchesWholePathsOnly(t *testing.T) {
	paths := []string{"a/b.txt", gateSource, "z.md"}
	for _, target := range []string{"a/b.txt", gateSource, "z.md"} {
		if !hasPath(paths, target) {
			t.Errorf("hasPath(%q) = false", target)
		}
	}
	for _, target := range []string{"", "b.txt", "a/", "a/b", "A/B.TXT", "z.md.bak"} {
		if hasPath(paths, target) {
			t.Errorf("hasPath(%q) = true", target)
		}
	}
	if hasPath(nil, gateSource) {
		t.Fatalf("hasPath(nil, %q) = true", gateSource)
	}
}

func TestRunRefusesAnIncompleteInvocation(t *testing.T) {
	root := t.TempDir()
	cases := []struct {
		name   string
		ctx    context.Context
		root   string
		stdout *bytes.Buffer
		stderr *bytes.Buffer
	}{
		{"no context", nil, root, &bytes.Buffer{}, &bytes.Buffer{}},
		{"no root", context.Background(), "", &bytes.Buffer{}, &bytes.Buffer{}},
		{"no stdout", context.Background(), root, nil, &bytes.Buffer{}},
		{"no stderr", context.Background(), root, &bytes.Buffer{}, nil},
	}
	for _, item := range cases {
		t.Run(item.name, func(t *testing.T) {
			var stdout, stderr io.Writer
			if item.stdout != nil {
				stdout = item.stdout
			}
			if item.stderr != nil {
				stderr = item.stderr
			}
			if code := Run(item.ctx, item.root, nil, stdout, stderr); code != 2 {
				t.Fatalf("Run = %d, want 2", code)
			}
			if item.stdout != nil && item.stdout.Len() != 0 {
				t.Fatalf("stdout = %q, want nothing", item.stdout.String())
			}
		})
	}
}

func TestRunRefusesUnexpectedArguments(t *testing.T) {
	for _, args := range [][]string{{"--help"}, {"--selftest", "extra"}, {"extra"}, {"", ""}} {
		var stdout, stderr bytes.Buffer
		if code := Run(context.Background(), t.TempDir(), args, &stdout, &stderr); code != 2 {
			t.Fatalf("Run(%v) = %d, want 2", args, code)
		}
		if !strings.Contains(stderr.String(), "usage: ra8ci no-unsafe-python-install") {
			t.Fatalf("Run(%v) stderr = %q, want the usage line", args, stderr.String())
		}
		if stdout.Len() != 0 {
			t.Fatalf("Run(%v) stdout = %q, want nothing", args, stdout.String())
		}
	}
}

func TestRunRefusesWhenTheScopeCannotBeEnumerated(t *testing.T) {
	var stdout, stderr bytes.Buffer
	root := plantTree(t, map[string]string{"README.md": "prose\n"})
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != 2 {
		t.Fatalf("Run = %d, want 2", code)
	}
	if !strings.Contains(stderr.String(), "cannot enumerate first-party files") {
		t.Fatalf("stderr = %q", stderr.String())
	}
	if stdout.Len() != 0 {
		t.Fatalf("stdout = %q, want nothing", stdout.String())
	}
}

func TestRunRefusesAScopeBelowTheFloor(t *testing.T) {
	var stdout, stderr bytes.Buffer
	root := plantRepo(t, map[string]string{"README.md": "prose\n", gateSource: "package unsafeinstall\n"})
	// A handful of files is not this repository. Answering clean here would
	// report a gate that scanned almost nothing as a passing gate.
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != 2 {
		t.Fatalf("Run = %d, want 2", code)
	}
	message := stderr.String()
	for _, want := range []string{"scope collapsed to 2 files", "at least 4000", gateSource} {
		if !strings.Contains(message, want) {
			t.Fatalf("stderr = %q, want it to name %q", message, want)
		}
	}
	if stdout.Len() != 0 {
		t.Fatalf("stdout = %q, want nothing", stdout.String())
	}
}

func TestRunSelfTestAnswersAheadOfTheScopeFloor(t *testing.T) {
	var stdout, stderr bytes.Buffer
	// Two files is far under the floor and the directory is not a repository,
	// so the self-test is reached only because it is judged first.
	root := plantTree(t, map[string]string{"README.md": "prose\n"})
	if code := Run(context.Background(), root, []string{"--selftest"}, &stdout, &stderr); code != 0 {
		t.Fatalf("Run --selftest = %d, stderr=%q", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "--selftest: PASS (7 cases)") {
		t.Fatalf("stdout = %q", stdout.String())
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want nothing", stderr.String())
	}
}
