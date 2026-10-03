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

// monitorWithDrillDirectory moves the fixture's restore drill receipt into a
// subdirectory held at directoryMode. The receipt itself stays 0640 and the
// pgBackRest executable and its directory stay protected, so a refusal can
// only come from the receipt directory rule under test.
func monitorWithDrillDirectory(t *testing.T, directoryMode os.FileMode) BackupMonitorConfig {
	t.Helper()
	config, _ := backupMonitorFixture(t, 0o750)
	directory := filepath.Join(filepath.Dir(config.RestoreDrillPath), "drills")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(directory, filepath.Base(config.RestoreDrillPath))
	if err := os.Rename(config.RestoreDrillPath, moved); err != nil {
		t.Fatal(err)
	}
	// Chmod after the move: a non-writable target directory would refuse the
	// rename itself, and the create mode is masked by the process umask.
	if err := os.Chmod(directory, directoryMode); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	config.RestoreDrillPath = moved
	return config
}

// A symlinked receipt directory is refused before its permissions are read,
// so the message names the shape rather than the mode.
func TestSymlinkedDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o750)
	real := filepath.Dir(config.RestoreDrillPath)
	link := filepath.Join(filepath.Dir(real), "drills-link")
	symlinkTest(t, real, link)
	config.RestoreDrillPath = filepath.Join(link, filepath.Base(config.RestoreDrillPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("symlinked restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}
