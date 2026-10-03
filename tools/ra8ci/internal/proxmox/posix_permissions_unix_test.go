//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package proxmox

import (
	os "os"
	filepath "path/filepath"
	strings "strings"
	testing "testing"
)

// A token file can pass every policy check on its metadata and still not open:
// mode 0o000 is private, regular and small. The refusal has to come from the
// read rather than the client starting up with an empty credential.
func TestATokenFileThatPassesPolicyAndStillWillNotOpenIsRefused(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root reads a mode 0o000 file regardless of its mode")
	}
	cfg := reviewedConfig(t)
	sealed := filepath.Join(t.TempDir(), "token")
	if err := os.WriteFile(sealed, []byte("ra8ci@pve!client=secret-token\n"), 0o000); err != nil {
		t.Fatal(err)
	}
	cfg.TokenFile = sealed

	err := refused(t, cfg)
	if !strings.Contains(err.Error(), "token file unreadable") {
		t.Errorf("refusal = %v, want the unreadable token file named", err)
	}
}
