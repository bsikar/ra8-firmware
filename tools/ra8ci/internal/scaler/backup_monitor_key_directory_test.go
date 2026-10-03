// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// monitorWithKeyDirectory moves the fixture's signing key into a subdirectory
// held at directoryMode. The key itself stays 0600 and every other input stays
// protected, so a refusal can only come from the key directory rule under test.
func monitorWithKeyDirectory(t *testing.T, directoryMode os.FileMode) BackupMonitorConfig {
	t.Helper()
	config, _ := backupMonitorFixture(t, 0o750)
	directory := filepath.Join(filepath.Dir(config.PrivateKeyPath), "keys")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(directory, filepath.Base(config.PrivateKeyPath))
	if err := os.Rename(config.PrivateKeyPath, moved); err != nil {
		t.Fatal(err)
	}
	// Chmod after the move: a non-writable target directory would refuse the
	// rename itself, and the create mode is masked by the process umask.
	if err := os.Chmod(directory, directoryMode); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	config.PrivateKeyPath = moved
	return config
}

func TestSymlinkedSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o750)
	real := filepath.Dir(config.PrivateKeyPath)
	link := filepath.Join(filepath.Dir(real), "keys-link")
	symlinkTest(t, real, link)
	config.PrivateKeyPath = filepath.Join(link, filepath.Base(config.PrivateKeyPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("symlinked signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}
