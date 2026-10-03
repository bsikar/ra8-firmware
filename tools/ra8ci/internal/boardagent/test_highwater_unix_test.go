//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"os"
	"path/filepath"
	"testing"
)

func newTestHighWater(t *testing.T) (HighWaterStore, string) {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(directory, "generation.state")
	state, err := NewFileHighWater(path, "ek-ra8d2")
	if err != nil {
		t.Fatal(err)
	}
	return state, path
}
