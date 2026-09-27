// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// script writes an extensionless file under root with the given first line.
func script(t *testing.T, root, rel, body string) string {
	t.Helper()
	path := filepath.Join(root, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0700); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestAHookScriptIsFoundByADirectoryScan(t *testing.T) {
	root := t.TempDir()
	script(t, root, "scripts/git/commit-msg", "#!/usr/bin/env bash\necho hi")
	code, _, stderr := scan(t, root, "scripts")
	if code != 1 {
		t.Fatalf("code = %d, want 1; stderr=%q", code, stderr)
	}
	if !strings.Contains(stderr, "commit-msg") {
		t.Fatalf("hook script not scanned: %q", stderr)
	}
}

func TestAHookScriptNamedDirectlyIsScanned(t *testing.T) {
	root := t.TempDir()
	script(t, root, "scripts/git/post-merge", "#!/bin/sh\nexit 0")
	code, _, stderr := scan(t, root, "scripts/git/post-merge")
	if code != 1 {
		t.Fatalf("code = %d, want 1; stderr=%q", code, stderr)
	}
}

func TestAScriptThatEndsInANewlineStillPasses(t *testing.T) {
	root := t.TempDir()
	script(t, root, "scripts/git/post-commit", "#!/bin/bash\nexit 0\n")
	code, stdout, stderr := scan(t, root, "scripts")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "1 file(s) scanned") {
		t.Fatalf("script not counted: %q", stdout)
	}
}

func TestEveryInterpreterTheHooksUse(t *testing.T) {
	root := t.TempDir()
	for name, first := range map[string]string{
		"with-sh":      "#!/bin/sh",
		"with-bash":    "#!/bin/bash",
		"with-zsh":     "#!/bin/zsh",
		"with-dash":    "#!/bin/dash",
		"with-env":     "#!/usr/bin/env bash",
		"with-python":  "#!/usr/bin/python",
		"with-python3": "#!/usr/bin/env python3",
	} {
		path := script(t, root, name, first+"\nbody")
		if !isScriptWithoutSuffix(path) {
			t.Fatalf("%s: %q not read as a script", name, first)
		}
	}
}

func TestAnExtensionlessFileThatIsNotAScriptStaysOut(t *testing.T) {
	root := t.TempDir()
	for name, body := range map[string]string{
		"LICENSE":  "no newline and no shebang",
		"fixture":  "\x00\x01binary",
		"NOTES":    "#not a shebang",
		"with-awk": "#!/usr/bin/awk -f",
	} {
		path := script(t, root, name, body)
		if isScriptWithoutSuffix(path) {
			t.Fatalf("%s was read as a script", name)
		}
	}
	code, stdout, stderr := scan(t, root, ".")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stdout=%q stderr=%q", code, stdout, stderr)
	}
	if !strings.Contains(stdout, "no files to scan") && !strings.Contains(stderr, "no files to scan") {
		t.Fatalf("non-scripts were pulled into scope: stdout=%q stderr=%q", stdout, stderr)
	}
}

func TestAFileWithASuffixIsJudgedBySuffixAlone(t *testing.T) {
	root := t.TempDir()
	shebangged := sourceFile(t, root, "notes.md", "#!/bin/sh\nno newline")
	if isScriptWithoutSuffix(shebangged) {
		t.Fatal("a suffixed file was read as an extensionless script")
	}
	code, _, stderr := scan(t, root, "notes.md")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stderr=%q", code, stderr)
	}
}

func TestAnExcludedScriptIsStillExcluded(t *testing.T) {
	root := t.TempDir()
	script(t, root, "libs/third_party/configure", "#!/bin/sh\nno newline")
	code, stdout, stderr := scan(t, root, "libs")
	if code != 0 {
		t.Fatalf("code = %d, want 0; stdout=%q stderr=%q", code, stdout, stderr)
	}
}

func TestADirectoryIsNotReadAsAScript(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "scripts", "git"), 0700); err != nil {
		t.Fatal(err)
	}
	if isScriptWithoutSuffix(filepath.Join(root, "scripts", "git")) {
		t.Fatal("a directory was read as a script")
	}
}
