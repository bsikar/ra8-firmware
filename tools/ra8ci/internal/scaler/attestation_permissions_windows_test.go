//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"os"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/privatefile"
)

func assertPublishedAttestationProtection(t *testing.T, path string, _ os.FileInfo) {
	t.Helper()
	if err := privatefile.CheckNoUntrustedWrite(path); err != nil {
		t.Fatalf("published attestation DACL permits untrusted writes: %v", err)
	}
}
