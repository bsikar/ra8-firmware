// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

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

// The derived scope is the one CI scans. A file that falls out of it is a file
// the ASCII rule stops applying to, silently, so what admits and what drops is
// pinned here rather than left to the gate's next reader.

func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable on this box")
	}
	root := t.TempDir()
	for name, contents := range files {
		full := filepath.Join(root, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("plant %s: %v", name, err)
		}
		if err := os.WriteFile(full, []byte(contents), 0o644); err != nil {
			t.Fatalf("plant %s: %v", name, err)
		}
	}
	command := exec.Command("git", "init", "-q", root)
	if output, err := command.CombinedOutput(); err != nil {
		t.Skipf("git init refused: %v: %s", err, output)
	}
	return root
}

// derivedScope answers what --all would scan, as a set, so a case names what it
// expects rather than an index into a slice.
func derivedScope(t *testing.T, root string) map[string]bool {
	t.Helper()
	targets, err := derivedTargets(context.Background(), root)
	if err != nil {
		t.Fatalf("derive scope: %v", err)
	}
	held := make(map[string]bool, len(targets))
	for _, target := range targets {
		held[target] = true
	}
	return held
}

func TestTheDerivedScopeKeepsFirstPartyTextAndScripts(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"README.md":              "prose\n",
		"apps/ui/main.c":         "int main(void){return 0;}\n",
		"apps/ui/main.h":         "#pragma once\n",
		"infra/deploy.yml":       "key: value\n",
		"docs/notes.txt":         "notes\n",
		"scripts/git/commit-msg": "#!/bin/sh\nexit 0\n",
		"scripts/report":         "#!/usr/bin/env python3\nprint(1)\n",
	})
	held := derivedScope(t, root)
	for _, want := range []string{
		"README.md", "apps/ui/main.c", "apps/ui/main.h", "infra/deploy.yml",
		"docs/notes.txt", "scripts/git/commit-msg", "scripts/report",
	} {
		if !held[want] {
			t.Errorf("derived scope dropped %s: %v", want, held)
		}
	}
}

func TestTheDerivedScopeReadsAnExtensionByItsLowercase(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"docs/SHOUTED.MD":   "prose\n",
		"apps/legacy.C":     "int main(void){return 0;}\n",
		"infra/pinned.YAML": "key: value\n",
	})
	held := derivedScope(t, root)
	for _, want := range []string{"docs/SHOUTED.MD", "apps/legacy.C", "infra/pinned.YAML"} {
		if !held[want] {
			t.Errorf("an uppercased extension was dropped: %s: %v", want, held)
		}
	}
}

func TestOnlyAnExtensionlessScriptJoinsOnItsFirstLine(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"scripts/shell":   "#!/bin/bash\nexit 0\n",
		"scripts/zshell":  "#!/usr/bin/env zsh\nexit 0\n",
		"scripts/dashing": "#!/bin/dash\nexit 0\n",
		"scripts/snake":   "#!/usr/bin/python\nprint(1)\n",
		"scripts/pearl":   "#!/usr/bin/perl\nprint 1;\n",
		"scripts/ruby":    "#!/usr/bin/env ruby\nputs 1\n",
		"LICENSE":         "All rights reserved\n",
		"scripts/late":    "not a shebang\n#!/bin/sh\n",
	})
	held := derivedScope(t, root)
	for _, want := range []string{"scripts/shell", "scripts/zshell", "scripts/dashing", "scripts/snake"} {
		if !held[want] {
			t.Errorf("an interpreter this gate reads was dropped: %s", want)
		}
	}
	for _, unwanted := range []string{"scripts/pearl", "scripts/ruby", "LICENSE", "scripts/late"} {
		if held[unwanted] {
			t.Errorf("%s is not a shell or python script and must stay out of scope", unwanted)
		}
	}
}

func TestAnUnknownExtensionStaysOutOfScopeWhateverItHolds(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"docs/diagram.svg": "<svg/>\n",
		"apps/ui/app.ts":   "export {};\n",
		"apps/ui/app.go":   "package ui\n",
		"docs/notes.rst":   "notes\n",
		"docs/keep.md":     "prose\n",
	})
	held := derivedScope(t, root)
	if !held["docs/keep.md"] {
		t.Fatalf("the fixture's one scanned file was dropped: %v", held)
	}
	for _, unwanted := range []string{"docs/diagram.svg", "apps/ui/app.ts", "apps/ui/app.go", "docs/notes.rst"} {
		if held[unwanted] {
			t.Errorf("%s has no scanned extension and must stay out of scope", unwanted)
		}
	}
}

func TestOnlyARegularFileIsDerived(t *testing.T) {
	root := plantRepo(t, map[string]string{"docs/real.md": "prose\n"})
	if err := os.Symlink(filepath.Join(root, "docs", "real.md"), filepath.Join(root, "docs", "linked.md")); err != nil {
		t.Skipf("symlinks are unavailable here: %v", err)
	}
	held := derivedScope(t, root)
	if !held["docs/real.md"] {
		t.Errorf("the real file was dropped: %v", held)
	}
	if held["docs/linked.md"] {
		t.Errorf("a symlink was derived; rewriting through one edits a file outside the scope: %v", held)
	}
}

func TestVendoredAndGeneratedTreesAreNeverDerived(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"libs/third_party/lib.c":               "int f(void){return 0;}\n",
		"apps/shared_libs/third_party/x.h":     "#pragma once\n",
		"libs/ra8_fonts/font.c":                "int g(void){return 0;}\n",
		"tools/vela/generated/model.py":        "x = 1\n",
		"libs/third_party_notes/README.md":     "ours\n",
		"libs/ra8_fonts_notes/README.md":       "ours\n",
		"tools/vela/generated_by_hand/keep.md": "ours\n",
	})
	held := derivedScope(t, root)
	for _, unwanted := range []string{
		"libs/third_party/lib.c", "apps/shared_libs/third_party/x.h",
		"libs/ra8_fonts/font.c", "tools/vela/generated/model.py",
	} {
		if held[unwanted] {
			t.Errorf("%s is not ours to rewrite and must stay out of scope", unwanted)
		}
	}
	for _, want := range []string{
		"libs/third_party_notes/README.md", "libs/ra8_fonts_notes/README.md",
		"tools/vela/generated_by_hand/keep.md",
	} {
		if !held[want] {
			t.Errorf("a prefix match on a path boundary swallowed one of ours: %s: %v", want, held)
		}
	}
}

func TestTheDerivedScopeIsSortedAcrossBothHalves(t *testing.T) {
	root := plantRepo(t, map[string]string{
		"zz.md":            "prose\n",
		"aa.md":            "prose\n",
		"mm.md":            "prose\n",
		"scripts/aardvark": "#!/bin/sh\nexit 0\n",
		"scripts/zebra":    "#!/bin/sh\nexit 0\n",
	})
	targets, err := derivedTargets(context.Background(), root)
	if err != nil {
		t.Fatalf("derive scope: %v", err)
	}
	want := []string{"aa.md", "mm.md", "scripts/aardvark", "scripts/zebra", "zz.md"}
	if len(targets) != len(want) {
		t.Fatalf("derived %v, want %v", targets, want)
	}
	for index, entry := range want {
		if targets[index] != entry {
			t.Fatalf("derived %v, want %v: the extensionless half is sorted WITH the suffixed half, not appended after it", targets, want)
		}
	}
}

func TestADirectoryThatIsNotARepositoryIsAnErrorNotAnEmptyScope(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable on this box")
	}
	root := t.TempDir()
	if err := os.WriteFile(filepath.Join(root, "notes.md"), []byte("prose\n"), 0o644); err != nil {
		t.Fatalf("plant: %v", err)
	}
	targets, err := derivedTargets(context.Background(), root)
	if err == nil {
		t.Fatalf("a scope derived outside a repository would scan nothing and call the tree clean: %v", targets)
	}
	if !strings.Contains(err.Error(), "git ls-files") {
		t.Errorf("refusal %q names neither the command nor the reason", err)
	}
	if targets != nil {
		t.Errorf("a refused derivation handed back %v", targets)
	}
}

func TestACancelledDerivationIsRefusedRatherThanShortened(t *testing.T) {
	root := plantRepo(t, map[string]string{"notes.md": "prose\n"})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	targets, err := derivedTargets(ctx, root)
	if err == nil {
		t.Fatalf("a cancelled derivation handed back a scope: %v", targets)
	}
}

func TestExcludedPathsAreJudgedByTheirDirectoriesAlone(t *testing.T) {
	for _, excluded := range []string{
		"libs/third_party/x.c",
		"apps/shared_libs/third_party/x.c",
		"libs/ra8_fonts/x.c",
		"tools/vela/generated/x.py",
		"build/x.md",
		"build-cov/x.md",
		"build_debug/x.md",
		"cmake-build-release/x.md",
		"docs/build/x.md",
		"tests/build-cov/x.md",
		"port/cmake-build-debug/x.md",
		"apps/deep/CMakeFiles/x.md",
		"libs/_deps/x.md",
		"tools/__pycache__/x.md",
		"apps/ui/node_modules/pkg/x.md",
	} {
		if !isExcluded(excluded) {
			t.Errorf("%s should be excluded", excluded)
		}
	}
	for _, kept := range []string{
		"README.md",
		"libs/third_party_notes/x.md",
		"libs/build_system_notes.md",
		"libs/deep/build/x.md",
		"infra/build-debug/x.md",
		"docs/build",
		"docs/CMakeFiles",
		"apps/ui/main.c",
	} {
		if isExcluded(kept) {
			t.Errorf("%s is ours and should stay in scope", kept)
		}
	}
}

func TestOnlyANamedTopLevelMakesADeepBuildDirectoryABuildTree(t *testing.T) {
	for _, root := range []string{"docs", "examples", "local-poc", "port", "tests", "tools", "apps"} {
		if !isBuildTreeRoot(root) {
			t.Errorf("%s is a build tree root", root)
		}
	}
	for _, root := range []string{"libs", "infra", "just", "scripts", "build", "", "Docs", "apps/ui"} {
		if isBuildTreeRoot(root) {
			t.Errorf("%s is not a build tree root", root)
		}
	}
}

func TestSortingIsStableEnoughToCompareTwoScans(t *testing.T) {
	var empty []string
	sortStrings(empty)
	sortStrings([]string{})
	sortStrings([]string{"only"})

	values := []string{"b.md", "a.md", "c.md", "a.md", "", "B.md"}
	sortStrings(values)
	want := []string{"", "B.md", "a.md", "a.md", "b.md", "c.md"}
	for index, entry := range want {
		if values[index] != entry {
			t.Fatalf("sorted %v, want %v (the order is byte-wise, so an uppercase name leads)", values, want)
		}
	}
}

func TestTheSelfTestRefusesAScopeTooSmallToTrust(t *testing.T) {
	root := plantRepo(t, map[string]string{"notes.md": "prose\n"})
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"--selftest"}, &stdout, &stderr); code != 2 {
		t.Fatalf("exit %d, want 2: a self-test that passes over a handful of files proves nothing", code)
	}
	if !strings.Contains(stderr.String(), "selftest FAILED") {
		t.Errorf("stderr %q does not say the self-test failed", stderr.String())
	}
	if !strings.Contains(stderr.String(), "floor is 2500") {
		t.Errorf("stderr %q does not name the floor the scope missed", stderr.String())
	}
	if strings.Contains(stdout.String(), "passed") {
		t.Errorf("stdout %q reports a pass the self-test did not give", stdout.String())
	}
}

func TestTheSelfTestRefusesARootItCannotDeriveAScopeFrom(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is unavailable on this box")
	}
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); code != 2 {
		t.Fatalf("exit %d, want 2", code)
	}
	if !strings.Contains(stderr.String(), "selftest FAILED") {
		t.Errorf("stderr %q does not say the self-test failed", stderr.String())
	}
	if strings.Contains(stdout.String(), "passed") {
		t.Errorf("stdout %q reports a pass over a root with no repository: %q", stdout.String(), stderr.String())
	}
}

func TestTheSelfTestIsNotCombinedWithAScan(t *testing.T) {
	root := plantRepo(t, map[string]string{"notes.md": "prose\n"})
	for _, args := range [][]string{
		{"--selftest", "--all"},
		{"--selftest", "--check"},
		{"--selftest", "--checkout"},
		{"--selftest", "notes.md"},
	} {
		var stdout, stderr bytes.Buffer
		if code := Run(context.Background(), root, args, &stdout, &stderr); code != 2 {
			t.Errorf("%v: exit %d, want 2", args, code)
		}
		if !strings.Contains(stderr.String(), "cannot be combined") {
			t.Errorf("%v: stderr %q does not name the conflict", args, stderr.String())
		}
		if stdout.Len() != 0 {
			t.Errorf("%v: wrote %q to stdout", args, stdout.String())
		}
	}
}

func TestAScanRefusesWhenItCannotBeTrusted(t *testing.T) {
	root := plantRepo(t, map[string]string{"notes.md": "prose\n"})
	var stdout, stderr bytes.Buffer
	if code := Run(context.Background(), root, []string{"--all", "--check"}, &stdout, &stderr); code != 2 {
		t.Fatalf("exit %d, want 2: a tree of one file is not the repository", code)
	}
	if !strings.Contains(stderr.String(), "refusing a vacuous scan") {
		t.Errorf("stderr %q does not say why the scan was refused", stderr.String())
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	var cancelledOut, cancelledErr bytes.Buffer
	if code := Run(ctx, root, []string{"--check", filepath.Join(root, "notes.md")}, &cancelledOut, &cancelledErr); code != 2 {
		t.Fatalf("exit %d, want 2 for a cancelled scan", code)
	}
	if !strings.Contains(cancelledErr.String(), "cancelled") {
		t.Errorf("stderr %q does not report the cancellation", cancelledErr.String())
	}

	var writer io.Writer
	if code := Run(context.Background(), root, []string{"--all"}, &stdout, writer); code != 2 {
		t.Errorf("exit %d, want 2 when there is nowhere to report a refusal", code)
	}
}
