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

// monitorWithBinaryDirectory moves the fixture's pgBackRest executable into a
// subdirectory held at directoryMode. The executable itself stays 0750, so a
// refusal can only come from the directory rule under test.
func monitorWithBinaryDirectory(t *testing.T, directoryMode os.FileMode) BackupMonitorConfig {
	t.Helper()
	config, _ := backupMonitorFixture(t, 0o750)
	directory := filepath.Join(filepath.Dir(config.PgBackRestPath), "bin")
	if err := os.Mkdir(directory, 0o700); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Join(directory, filepath.Base(config.PgBackRestPath))
	if err := os.Rename(config.PgBackRestPath, moved); err != nil {
		t.Fatal(err)
	}
	// Chmod after the move: a non-writable target directory would refuse the
	// rename itself, and the create mode is masked by the process umask.
	if err := os.Chmod(directory, directoryMode); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	config.PgBackRestPath = moved
	return config
}

func TestPgBackRestDirectoryThatIsASymlinkIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o750)
	directory := filepath.Dir(config.PgBackRestPath)
	link := filepath.Join(filepath.Dir(directory), "bin-link")
	symlinkTest(t, directory, link)
	config.PgBackRestPath = filepath.Join(link, filepath.Base(config.PgBackRestPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a symlinked pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}
