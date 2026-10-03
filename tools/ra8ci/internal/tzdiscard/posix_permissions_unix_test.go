//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package tzdiscard

import (
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

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
