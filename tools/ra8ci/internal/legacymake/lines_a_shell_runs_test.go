// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package legacymake

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// word keeps the banned spelling out of this file as a literal, the way the
// detector's own self-test does, so scanning this repository does not report
// the tests that pin the scanner.
func word() string { return "ma" + "ke" }

// scanTree writes files into a temporary root and scans exactly those paths.
func scanTree(t *testing.T, files map[string]string) []finding {
	t.Helper()
	root := t.TempDir()
	paths := make([]string, 0, len(files))
	for rel, body := range files {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("mkdir %s: %v", rel, err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatalf("write %s: %v", rel, err)
		}
		paths = append(paths, rel)
	}
	findings, err := scan(context.Background(), root, paths)
	if err != nil {
		t.Fatalf("scan: %v", err)
	}
	return findings
}

func TestAJustRecipeLineIsACommand(t *testing.T) {
	findings := scanTree(t, map[string]string{
		"just/ci.just": "ci-native:\n    " + word() + " -C apps/blink build\n",
	})
	if len(findings) != 1 {
		t.Fatalf("findings = %+v, want one", findings)
	}
	if findings[0].line != 2 || !strings.Contains(findings[0].command, "-C") {
		t.Errorf("finding = %+v, want the recipe line with its target", findings[0])
	}
}

func TestTheRootJustfileIsACommandFile(t *testing.T) {
	findings := scanTree(t, map[string]string{
		"justfile": "default:\n    " + word() + " ci\n",
	})
	if len(findings) != 1 {
		t.Fatalf("findings = %+v, want one", findings)
	}
}

func TestAGitHookWithNoSuffixIsACommandFile(t *testing.T) {
	findings := scanTree(t, map[string]string{
		"scripts/git/post-commit": "#!/usr/bin/env sh\n" + word() + " sbom\n",
	})
	if len(findings) != 1 {
		t.Fatalf("findings = %+v, want one", findings)
	}
	if findings[0].path != "scripts/git/post-commit" {
		t.Errorf("path = %q", findings[0].path)
	}
}

func TestProseStaysProse(t *testing.T) {
	findings := scanTree(t, map[string]string{
		"docs/build.md": "The vendored library ships a GNUmakefile.\n" + word() + " ci\n",
	})
	if len(findings) != 0 {
		t.Fatalf("findings = %+v, want none: a bare line in prose is not a command", findings)
	}
}

func TestAShellShebangDoesNotPromoteASuffixedFile(t *testing.T) {
	if runsCommands("scripts/report.py", []byte("#!/bin/bash\n")) {
		t.Error("a file with a suffix must be judged by its suffix alone")
	}
}

func TestSuffixesTheShellRuns(t *testing.T) {
	for _, rel := range []string{"scripts/x.sh", "scripts/x.bash", "scripts/x.zsh", "scripts/x.ksh", ".github/workflows/ci.yml", "x.yaml", "just/a.just", "just/A.JUST", "Dockerfile", "tools/mcp/Dockerfile"} {
		if !runsCommands(rel, nil) {
			t.Errorf("runsCommands(%q) = false, want true", rel)
		}
	}
	for _, rel := range []string{"README.md", "docs/a.rst", "CMakePresets.json", ".clangd", "x.mdx", "scripts/a.py"} {
		if runsCommands(rel, nil) {
			t.Errorf("runsCommands(%q) = true, want false", rel)
		}
	}
}

func TestShellShebangForms(t *testing.T) {
	for _, head := range []string{"#!/bin/sh\n", "#!/bin/bash -p\n", "#!/usr/bin/env sh\n", "#!/usr/bin/env -S bash -eu\n", "#!/bin/zsh\n", "#!/usr/bin/dash\n", "#!/bin/bash"} {
		if !hasShellShebang([]byte(head)) {
			t.Errorf("hasShellShebang(%q) = false, want true", head)
		}
	}
	for _, head := range []string{"#!/usr/bin/env python3\n", "#!/usr/bin/perl\n", "#! /usr/bin/env node\n", "", "\n#!/bin/sh\n", "# !/bin/sh\n", "VERSION=1\n"} {
		if hasShellShebang([]byte(head)) {
			t.Errorf("hasShellShebang(%q) = true, want false", head)
		}
	}
}

func TestShebangIsReadFromTheFirstLineOnly(t *testing.T) {
	padded := "#!/bin/sh" + strings.Repeat(" ", shebangLookback*2) + "\n"
	if !hasShellShebang([]byte(padded)) {
		t.Error("a long first line must still be read up to its interpreter")
	}
	buried := strings.Repeat("x", shebangLookback*2) + "\n#!/bin/sh\n"
	if hasShellShebang([]byte(buried)) {
		t.Error("a shebang that is not the first line is not a shebang")
	}
}

func TestACarriageReturnEndsTheShebangLine(t *testing.T) {
	if !hasShellShebang([]byte("#!/bin/bash\r\n" + word() + " ci\r\n")) {
		t.Error("a CRLF script must be read as a script")
	}
}

func TestASuffixlessFileWithNoShebangIsNotACommandFile(t *testing.T) {
	if runsCommands("VERSION", []byte("1.4.2\n")) {
		t.Error("VERSION is data, not commands")
	}
}
