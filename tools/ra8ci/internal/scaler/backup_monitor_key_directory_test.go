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

func TestProtectedSigningKeyDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o750)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("protected signing key directory was refused: %v", err)
	}
}

// The monitor owns the key it signs with, so owning its directory outright is
// the ordinary deployment.
func TestOwnerWritableSigningKeyDirectoryIsStillAccepted(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o700)
	if err := RefreshBackupAttestation(context.Background(), config); err != nil {
		t.Fatalf("owner-writable signing key directory was refused: %v", err)
	}
}

func TestGroupWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o770)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("group-writable signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestWorldWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o757)
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("world-writable signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must not be group or world writable") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// Same call as every other monitor input: the sticky bit buys no exception.
func TestStickyWorldWritableSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, os.ModeSticky|0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("sticky world-writable signing key directory was accepted")
	}
}

func TestWritableSigningKeyDirectorySignsNothing(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o777)
	if err := RefreshBackupAttestation(context.Background(), config); err == nil {
		t.Fatal("world-writable signing key directory was accepted")
	}
	if _, err := os.Stat(config.AttestationPath); !os.IsNotExist(err) {
		t.Fatal("a refused run published an attestation")
	}
}

// A readable key file is still refused for its own reason when the directory
// holding it is protected.
func TestReadableSigningKeyStillNamesTheKey(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o750)
	if err := os.Chmod(config.PrivateKeyPath, 0o644); err != nil {
		t.Fatal(err)
	}
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("a group and world readable signing key was accepted")
	}
	if !strings.Contains(err.Error(), "bounded private regular file") {
		t.Fatalf("the pre-existing rule no longer names its own reason: %v", err)
	}
}

func TestSymlinkedSigningKeyDirectoryIsRefused(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o750)
	real := filepath.Dir(config.PrivateKeyPath)
	link := filepath.Join(filepath.Dir(real), "keys-link")
	if err := os.Symlink(real, link); err != nil {
		t.Fatal(err)
	}
	config.PrivateKeyPath = filepath.Join(link, filepath.Base(config.PrivateKeyPath))
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("symlinked signing key directory was accepted")
	}
	if !strings.Contains(err.Error(), "signing key directory must be a real directory") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

// The receipt directory rule is judged before the key is read, so a run with
// both wrong still names the receipt directory.
func TestDrillDirectoryIsStillJudgedBeforeTheKeyDirectory(t *testing.T) {
	config := monitorWithKeyDirectory(t, 0o777)
	if err := os.Chmod(filepath.Dir(config.RestoreDrillPath), 0o777); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(filepath.Dir(config.RestoreDrillPath), 0o700) })
	err := RefreshBackupAttestation(context.Background(), config)
	if err == nil {
		t.Fatal("two writable input directories were accepted")
	}
	if !strings.Contains(err.Error(), "receipt directory must not be group or world writable") {
		t.Fatalf("the earlier rule did not answer first: %v", err)
	}
}
