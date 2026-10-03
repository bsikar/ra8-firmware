// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestCheckRequiresOwnerOnlyModeOnUnix(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows uses DACL validation")
	}
	path := filepath.Join(t.TempDir(), "secret")
	if err := os.WriteFile(path, []byte("secret"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := Check(path); err != nil {
		t.Fatalf("owner-only file refused: %v", err)
	}
	for _, mode := range []os.FileMode{0o640, 0o604, 0o666} {
		if err := os.Chmod(path, mode); err != nil {
			t.Fatal(err)
		}
		if err := Check(path); err == nil {
			t.Fatalf("mode %04o was accepted", mode)
		}
	}
}

func TestCheckRefusesDirectories(t *testing.T) {
	if err := Check(t.TempDir()); err == nil {
		t.Fatal("directory was accepted as a private file")
	}
}
