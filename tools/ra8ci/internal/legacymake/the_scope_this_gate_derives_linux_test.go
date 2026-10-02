// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

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

// scopeOf plants a repository holding exactly these paths and answers what the
// gate derives from it, with the gate's own source removed so a case reads as
// the keep rule it is testing.
func scopeOf(t *testing.T, paths ...string) []string {
	t.Helper()
	files := map[string]string{}
	for _, rel := range paths {
		files[rel] = "text\n"
	}
	var kept []string
	for _, rel := range derivedScope(t, plantRepo(t, files)) {
		if rel != gateSource {
			kept = append(kept, rel)
		}
	}
	return kept
}

func TestScopeKeepsAuthoredAutomationAndProse(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"README.md":                       "prose\n",
		"docs/guide.mdx":                  "prose\n",
		"docs/legacy.rst":                 "prose\n",
		"justfile":                        "build:\n",
		".clangd":                         "Diagnostics:\n",
		".cppcheck-suppressions":          "*\n",
		".env.example":                    "KEY=\n",
		"CMakePresets.json":               "{}\n",
		".devcontainer/devcontainer.json": "{}\n",
		".github/workflows/ci.yml":        "on: push\n",
		".vscode/tasks.json":              "{}\n",
		"just/build.just":                 "build:\n",
		"scripts/setup.sh":                "#!/bin/sh\n",
		"tools/mcp/server.py":             "print()\n",
		"apps/blink/Dockerfile":           "FROM x\n",
		gateSource:                        "package legacymake\n",
		// Neither authored automation nor prose: source, data and a plain
		// text file outside the scoped trees are not this gate's business.
		"apps/blink/main.c":  "int main(void){return 0;}\n",
		"tools/other/run.py": "print()\n",
		"notes.txt":          "text\n",
		"data/values.json":   "{}\n",
	})
	want := []string{
		".clangd",
		".cppcheck-suppressions",
		".devcontainer/devcontainer.json",
		".env.example",
		".github/workflows/ci.yml",
		".vscode/tasks.json",
		"CMakePresets.json",
		"README.md",
		"apps/blink/Dockerfile",
		"docs/guide.mdx",
		"docs/legacy.rst",
		"just/build.just",
		"justfile",
		"scripts/setup.sh",
		"tools/mcp/server.py",
		gateSource,
	}
	got := derivedScope(t, root)
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeNamesTheExactFilesByWholePath(t *testing.T) {
	// The exact-file table is keyed by the path from the repository root, not
	// by base name, so a nested copy of one of those names is not scoped.
	got := scopeOf(t, ".clangd", "sub/.clangd", "justfile", "apps/blink/justfile", "CMakePresets.json", "sub/CMakePresets.json")
	want := []string{".clangd", "CMakePresets.json", "justfile"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeTakesDockerfileByBaseNameAtAnyDepth(t *testing.T) {
	got := scopeOf(t, "Dockerfile", "apps/blink/ci/Dockerfile", "Dockerfile.dev", "docker/dockerfile", "MyDockerfile")
	want := []string{"Dockerfile", "apps/blink/ci/Dockerfile"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeReadsProseSuffixesWithoutRegardToCase(t *testing.T) {
	got := scopeOf(t, "README.MD", "docs/A.Mdx", "docs/B.RST", "docs/c.markdown", "docs/d.txt")
	want := []string{"README.MD", "docs/A.Mdx", "docs/B.RST"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopePrefixesAreAnchoredAtTheStart(t *testing.T) {
	// Each near miss shares a leading run of characters with a scoped prefix
	// without being under it, which is what holds the prefix anchored.
	got := scopeOf(t,
		"scripts/setup.sh", "apps/scripts/setup.sh", "scripts-old/setup.sh",
		"just/build.just", "adjust/build.just",
		"tools/mcp/server.py", "tools/mcpx/server.py", "other/tools/mcp/server.py",
		".github/workflows/ci.yml", ".github/actions/thing/action.yml",
	)
	want := []string{".github/workflows/ci.yml", "just/build.just", "scripts/setup.sh", "tools/mcp/server.py"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeTakesTopLevelGithubBaselineText(t *testing.T) {
	// A baseline text file sitting directly in .github/ is authored data this
	// gate reads; the rule is deliberately narrow, so depth, the name and the
	// suffix each disqualify on their own.
	got := scopeOf(t,
		".github/coverage-baseline.txt",
		".github/baseline.txt",
		".github/nested/coverage-baseline.txt",
		".github/coverage-baseline.text",
		".github/coverage.txt",
		"baseline.txt",
	)
	want := []string{".github/baseline.txt", ".github/coverage-baseline.txt"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("scope = %v, want %v", got, want)
	}
}

func TestScopeDropsVendoredTreesEvenWhenTheyWouldOtherwiseBeKept(t *testing.T) {
	excludedPaths := []string{
		"docs/sbom/upstream/pkg.md",
		"libs/third_party/README.md",
		"apps/shared_libs/third_party/README.md",
		"port/netxduo/README.md",
		"port/nimble/README.md",
		"port/threadx/README.md",
		"port/usbx/README.md",
		"tests/fixtures/sample.md",
	}
	kept := []string{
		"docs/sbom/inventory.md",
		"libs/third_partytools/README.md",
		"apps/shared_libs/first_party/README.md",
		"apps/port/threadx/README.md",
		"tests/fixtures.md",
	}
	got := scopeOf(t, append(append([]string{}, excludedPaths...), kept...)...)
	if !reflect.DeepEqual(got, []string{
		"apps/port/threadx/README.md",
		"apps/shared_libs/first_party/README.md",
		"docs/sbom/inventory.md",
		"libs/third_partytools/README.md",
		"tests/fixtures.md",
	}) {
		t.Fatalf("scope = %v", got)
	}
	for _, rel := range excludedPaths {
		if !isExcluded(rel) {
			t.Errorf("isExcluded(%q) = false", rel)
		}
	}
	for _, rel := range kept {
		if isExcluded(rel) {
			t.Errorf("isExcluded(%q) = true", rel)
		}
	}
}

func TestScopeCarriesTheGateSourceGitWouldHide(t *testing.T) {
	// The gate's own path is what the scope floor is checked against, so it is
	// added by name after the enumeration and an ignore rule does not remove
	// it. A .go file is scoped no other way.
	root := plantRepo(t, map[string]string{
		".gitignore": "tools/\n",
		"README.md":  "prose\n",
		gateSource:   "package legacymake\n",
	})
	got := derivedScope(t, root)
	if !hasPath(got, gateSource) {
		t.Fatalf("scope = %v, want it to carry %s", got, gateSource)
	}
}

func TestScopeDoesNotInventAnAbsentGateSource(t *testing.T) {
	got := derivedScope(t, plantRepo(t, map[string]string{"README.md": "prose\n"}))
	if hasPath(got, gateSource) {
		t.Fatalf("scope = %v, want no %s", got, gateSource)
	}
}

func TestScopeHonoursGitignoreAndStillTakesUntrackedFiles(t *testing.T) {
	root := plantRepo(t, map[string]string{
		".gitignore":       "ignored.md\nbuild/\n",
		"ignored.md":       "prose\n",
		"build/notes.md":   "prose\n",
		"untracked.md":     "prose\n",
		"scripts/setup.sh": "#!/bin/sh\n",
	})
	got := derivedScope(t, root)
	for _, rel := range []string{"ignored.md", "build/notes.md"} {
		if hasPath(got, rel) {
			t.Errorf("scope holds ignored %s", rel)
		}
	}
	// Nothing is committed in this fixture, so everything kept arrives through
	// --others: an uncommitted tree is still scanned.
	for _, rel := range []string{"untracked.md", "scripts/setup.sh"} {
		if !hasPath(got, rel) {
			t.Errorf("scope drops %s", rel)
		}
	}
}

func TestScopeTakesOnlyWhatStatsAsARegularFile(t *testing.T) {
	root := plantRepo(t, map[string]string{"real.md": "prose\n"})
	if err := os.Symlink(filepath.Join(root, "real.md"), filepath.Join(root, "alias.md")); err != nil {
		t.Skipf("symlink: %v", err)
	}
	if err := os.Symlink(filepath.Join(root, "gone.md"), filepath.Join(root, "broken.md")); err != nil {
		t.Skipf("symlink: %v", err)
	}
	if err := syscall.Mkfifo(filepath.Join(root, "pipe.md"), 0o644); err != nil {
		t.Skipf("mkfifo: %v", err)
	}
	got := derivedScope(t, root)
	// os.Stat follows the link, so a symlink to a readable regular file is read
	// as that file; a broken link and a named pipe are passed over rather than
	// failing the whole enumeration.
	if !hasPath(got, "alias.md") {
		t.Errorf("scope drops a symlink to a regular file: %v", got)
	}
	for _, rel := range []string{"broken.md", "pipe.md"} {
		if hasPath(got, rel) {
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
		t.Fatalf("paths = %v, want nil", paths)
	}
	// An empty scope would read as a clean tree, so the failure is named.
	if !strings.Contains(err.Error(), "git ls-files") {
		t.Fatalf("error = %q, want it to name git ls-files", err)
	}
}

func TestHasPathMatchesWholePathsOnly(t *testing.T) {
	paths := []string{"README.md", gateSource, "just/build.just"}
	for _, target := range paths {
		if !hasPath(paths, target) {
			t.Errorf("hasPath(%q) = false", target)
		}
	}
	for _, target := range []string{"", "README", "readme.md", "just/", "build.just", gateSource + "x"} {
		if hasPath(paths, target) {
			t.Errorf("hasPath(%q) = true", target)
		}
	}
	if hasPath(nil, gateSource) {
		t.Fatalf("hasPath(nil, gate source) = true")
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
			if item.stderr != nil && !strings.Contains(item.stderr.String(), "invalid input") {
				t.Fatalf("stderr = %q, want the invalid-input refusal", item.stderr.String())
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
		if !strings.Contains(stderr.String(), "usage: ra8ci legacy-make") {
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
	if !strings.Contains(stderr.String(), "cannot enumerate tracked files") {
		t.Fatalf("stderr = %q", stderr.String())
	}
	if stdout.Len() != 0 {
		t.Fatalf("stdout = %q, want nothing", stdout.String())
	}
}

func TestRunRefusesAScopeBelowTheFloor(t *testing.T) {
	var stdout, stderr bytes.Buffer
	root := plantRepo(t, map[string]string{"README.md": "prose\n", gateSource: "package legacymake\n"})
	// A handful of files is not this repository. Answering clean here would
	// report a gate that scanned almost nothing as a passing gate.
	if code := Run(context.Background(), root, nil, &stdout, &stderr); code != 2 {
		t.Fatalf("Run = %d, want 2", code)
	}
	message := stderr.String()
	for _, want := range []string{"scope collapsed to 2 file(s)", "at least 650", gateSource} {
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
	// One file, and not a repository at all, so the self-test is reached only
	// because it is judged before the scope is ever derived.
	root := plantTree(t, map[string]string{"README.md": "prose\n"})
	if code := Run(context.Background(), root, []string{"--selftest"}, &stdout, &stderr); code != 0 {
		t.Fatalf("Run --selftest = %d, stderr=%q", code, stderr.String())
	}
	if !strings.Contains(stdout.String(), "--selftest: PASS (19 both-direction cases)") {
		t.Fatalf("stdout = %q", stdout.String())
	}
	if stderr.Len() != 0 {
		t.Fatalf("stderr = %q, want nothing", stderr.String())
	}
}
