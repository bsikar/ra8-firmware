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

func TestProtectedPgBackRestDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected executable directory was refused: %v", err)
	}
}

func TestOwnerWritablePgBackRestDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable executable directory was refused: %v", err)
	}
}

func TestGroupWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// A sticky bit stops another account unlinking a file it does not own, so a
// 1777 directory does block the substitution this rule is about. It is still
// refused: every other monitor input draws the line at group and other write
// with no exception, and a privileged executable does not belong in a shared
// scratch directory. Deleting this test and masking os.ModeSticky is the one
// change if that judgement is ever revisited.
func TestStickyWorldWritablePgBackRestDirectoryIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o777|os.ModeSticky)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("sticky world-writable pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWritablePgBackRestDirectorySignsNothing(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable pgBackRest directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

func TestPgBackRestDirectoryThatIsASymlinkIsRefused(t *testing.T) {
	config := monitorWithBinaryDirectory(t, 0o750)
	directory := filepath.Dir(config.PgBackRestPath)
	link := filepath.Join(filepath.Dir(directory), "bin-link")
	if err := os.Symlink(directory, link); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	config.PgBackRestPath = filepath.Join(link, filepath.Base(config.PgBackRestPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a symlinked pgBackRest directory was accepted")
	}
	if !strings.Contains(err.Error(), "directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// The executable rule keeps its own reason when both are wrong, so an operator
// is told about the file in front of them rather than its parent.
func TestWritableExecutableStillNamesTheExecutable(t *testing.T) {
	config, _ := backupMonitorFixture(t, 0o777)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable pgBackRest executable was accepted")
	}
	if !strings.Contains(err.Error(), "executable must not be group or world writable") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}
