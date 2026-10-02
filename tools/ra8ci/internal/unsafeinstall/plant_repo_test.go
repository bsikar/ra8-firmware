// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package unsafeinstall

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

// plantTree writes every named file under a fresh directory and answers its root.
func plantTree(t *testing.T, files map[string]string) string {
	t.Helper()
	root := t.TempDir()
	for rel, contents := range files {
		full := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatalf("mkdir for %s: %v", rel, err)
		}
		if err := os.WriteFile(full, []byte(contents), 0o644); err != nil {
			t.Fatalf("write %s: %v", rel, err)
		}
	}
	return root
}

// plantRepo plants the files and makes the root a Git repository. The gate only
// runs `git ls-files --cached --others --exclude-standard`, so untracked files
// answer and neither a commit nor a configured identity is needed.
func plantRepo(t *testing.T, files map[string]string) string {
	t.Helper()
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not available on this box")
	}
	root := plantTree(t, files)
	cmd := exec.Command("git", "init", "-q", root)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Skipf("git init: %v: %s", err, out)
	}
	return root
}
