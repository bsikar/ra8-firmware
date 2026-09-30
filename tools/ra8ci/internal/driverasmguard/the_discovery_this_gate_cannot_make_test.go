// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package driverasmguard

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Discovery is how the gate learns what to judge, so a discovery that fails
// has to be a refusal. The failure to avoid is the quiet one: a gate that
// read the error as an empty list would announce a clean pass over zero
// driver translation units, and a reviewer would take that as the drivers
// having been checked.
func TestADiscoveryTheGateCannotMakeIsARefusalNotACleanPass(t *testing.T) {
	// A path holding an unterminated character class is a legal directory
	// name and a malformed glob pattern, so the directory stats fine and
	// the search over it cannot be built.
	root := filepath.Join(t.TempDir(), "board[rev")
	if err := os.MkdirAll(filepath.Join(root, "libs", "ra8_hal", "src"), 0o755); err != nil {
		t.Fatal(err)
	}
	if info, err := os.Stat(filepath.Join(root, "libs", "ra8_hal", "src")); err != nil || !info.IsDir() {
		t.Fatalf("the driver directory is not there to be found: %v", err)
	}

	got := guard(t, context.Background(), root)
	if got.code != 2 {
		t.Fatalf("exit = %d, want 2: %+v", got.code, got)
	}
	if !strings.Contains(got.stderr, "driver discovery failed") {
		t.Fatalf("the refusal does not name the discovery: %q", got.stderr)
	}
	if strings.Contains(got.stdout, "PASS") {
		t.Fatalf("a failed discovery was announced as a pass: %q", got.stdout)
	}
}
