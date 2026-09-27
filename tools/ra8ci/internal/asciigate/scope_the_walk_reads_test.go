// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package asciigate

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"
)

func writeFileAt(t *testing.T, dir, name, contents string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(contents), 0700); err != nil {
		t.Fatal(err)
	}
	return path
}

func walked(t *testing.T, dir string) map[string]bool {
	t.Helper()
	targets, err := walkTargets(dir)
	if err != nil {
		t.Fatalf("walk %s: %v", dir, err)
	}
	found := map[string]bool{}
	for _, target := range targets {
		relative, err := filepath.Rel(dir, target)
		if err != nil {
			t.Fatal(err)
		}
		found[filepath.ToSlash(relative)] = true
	}
	return found
}

func TestTheWalkReadsTheExtensionlessScriptsTheDerivedScopeReads(t *testing.T) {
	dir := t.TempDir()
	writeFileAt(t, dir, "git/commit-msg", "#!/bin/sh\n# em dash\u2014here\n")
	writeFileAt(t, dir, "release", "#!/usr/bin/env python3\n# minus \u00b1 one\n")
	writeFileAt(t, dir, "nested/tools/flash", "#!/bin/bash\necho ok\n")

	found := walked(t, dir)
	for _, want := range []string{"git/commit-msg", "release", "nested/tools/flash"} {
		if !found[want] {
			t.Errorf("a script the derived scope reads is invisible to the walk: %s", want)
		}
	}
}

func TestTheWalkStillSkipsWhatIsNotAScriptAndNotText(t *testing.T) {
	dir := t.TempDir()
	writeFileAt(t, dir, "LICENSE", "All rights reserved\u2014see below\n")
	writeFileAt(t, dir, "notes", "no shebang here\n")
	writeFileAt(t, dir, "image.png", "\x89PNG\r\n")
	writeFileAt(t, dir, "run.bin", "#!/bin/sh\n")

	found := walked(t, dir)
	for _, unwanted := range []string{"LICENSE", "notes", "image.png", "run.bin"} {
		if found[unwanted] {
			t.Errorf("the walk read a file outside the rule's scope: %s", unwanted)
		}
	}
}

func TestTheWalkKeepsReadingNamedTextExtensions(t *testing.T) {
	dir := t.TempDir()
	writeFileAt(t, dir, "doc.md", "text\n")
	writeFileAt(t, dir, "conf.YAML", "text\n")
	writeFileAt(t, dir, "third_party/vendor.md", "text\n")
	writeFileAt(t, dir, "fixtures/sample.md", "text\n")

	found := walked(t, dir)
	if !found["doc.md"] || !found["conf.YAML"] {
		t.Errorf("a named text extension was dropped: %v", found)
	}
	if found["third_party/vendor.md"] || found["fixtures/sample.md"] {
		t.Errorf("an excluded directory was walked: %v", found)
	}
}

func TestAnExcludedDirectoryStillHidesItsScripts(t *testing.T) {
	dir := t.TempDir()
	writeFileAt(t, dir, "third_party/configure", "#!/bin/sh\ndash \u2014 here\n")
	if found := walked(t, dir); found["third_party/configure"] {
		t.Errorf("a script under an excluded directory was walked: %v", found)
	}
}

func TestAScriptWithNonASCIIFailsASubtreeCheckTheWayItFailsCI(t *testing.T) {
	root := t.TempDir()
	scripts := filepath.Join(root, "scripts")
	writeFileAt(t, scripts, "git/commit-msg", "#!/bin/sh\n# em dash\u2014here\n")

	if code := Run(context.Background(), root, []string{"--check", scripts}, io.Discard, io.Discard); code != 1 {
		t.Fatalf("subtree check of a script carrying a non-ASCII dash = %d, want 1", code)
	}
	if code := Run(context.Background(), root, []string{scripts}, io.Discard, io.Discard); code != 0 {
		t.Fatalf("subtree rewrite = %d, want 0", code)
	}
	contents, err := os.ReadFile(filepath.Join(scripts, "git", "commit-msg"))
	if err != nil || string(contents) != "#!/bin/sh\n# em dash--here\n" {
		t.Fatalf("rewritten script = %q, %v", contents, err)
	}
	if code := Run(context.Background(), root, []string{"--check", scripts}, io.Discard, io.Discard); code != 0 {
		t.Fatalf("check after the rewrite = %d, want 0", code)
	}
}

func TestScopeReadsTheSameShebangsTheDerivedScopeDoes(t *testing.T) {
	dir := t.TempDir()
	for name, contents := range map[string]string{
		"sh":     "#!/bin/sh\n",
		"bash":   "#!/usr/bin/env bash\n",
		"zsh":    "#!/bin/zsh -e\n",
		"dash":   "#!/bin/dash\n",
		"python": "#!/usr/bin/python3\n",
		"env":    "#!/usr/bin/env python\n",
	} {
		path := writeFileAt(t, dir, name, contents)
		if !inWalkScope(path) {
			t.Errorf("%s: the walk refuses a script the derived scope reads", name)
		}
	}
	for name, contents := range map[string]string{
		"perl":    "#!/usr/bin/perl\n",
		"node":    "#!/usr/bin/env node\n",
		"nothing": "plain text\n",
		"late":    "\n#!/bin/sh\n",
	} {
		path := writeFileAt(t, dir, name, contents)
		if inWalkScope(path) {
			t.Errorf("%s: the walk read a file the derived scope leaves alone", name)
		}
	}
}
