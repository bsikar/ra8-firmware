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

func TestProtectedDrillReceiptDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected restore drill receipt directory was refused: %v", err)
	}
}

// The drill runner is a different identity from the monitor, so it owns the
// directory it publishes into and needs no group write to do it.
func TestOwnerWritableDrillReceiptDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable restore drill receipt directory was refused: %v", err)
	}
}

func TestGroupWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// The sticky bit does block this substitution, but every other monitor input
// draws the line at group or other write with no exception, and a receipt the
// whole gate ages does not belong in shared scratch. Pinned so a later change
// is deliberate.
func TestStickyWorldWritableDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable restore drill receipt directory was accepted")
	}
}

func TestWritableDrillReceiptDirectorySignsNothing(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable restore drill receipt directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

// A writable receipt file is still refused for its own reason when the
// directory holding it is protected.
func TestWritableDrillReceiptStillNamesTheReceipt(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o750)
	if err := os.Chmod(config.RestoreDrillPath, 0o666); err != nil {
		t.Fatal(err)
	}
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group and world writable restore drill receipt was accepted")
	}
	if !strings.Contains(err.Error(), "bounded, protected regular file") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

// A symlinked receipt directory is refused before its permissions are read,
// so the message names the shape rather than the mode.
func TestSymlinkedDrillReceiptDirectoryIsRefused(t *testing.T) {
	config := monitorWithDrillDirectory(t, 0o750)
	real := filepath.Dir(config.RestoreDrillPath)
	link := filepath.Join(filepath.Dir(real), "drills-link")
	if err := os.Symlink(real, link); err != nil {
		t.Fatal(err)
	}
	config.RestoreDrillPath = filepath.Join(link, filepath.Base(config.RestoreDrillPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("symlinked restore drill receipt directory was accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}
