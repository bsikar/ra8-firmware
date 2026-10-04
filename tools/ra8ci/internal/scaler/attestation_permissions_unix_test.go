//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"testing"
)

func assertPublishedAttestationProtection(t *testing.T, _ string, info os.FileInfo) {
	t.Helper()
	if info.Mode().Perm() != 0o640 {
		t.Fatalf("published mode %v, want 0640", info.Mode().Perm())
	}
}
